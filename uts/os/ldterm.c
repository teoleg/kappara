/*
 * kernel/ldterm.c -- SVR4 line discipline (ldterm STREAMS module)
 *
 * Pushed onto a tty stream stack via I_PUSH "ldterm".  Below the
 * stream head, above the tty driver.  Handles the four pieces of
 * "cooked mode" the SVR4 tty subsystem leaves to a module instead
 * of baking into the head or driver:
 *
 *   - Input signal generation (ISIG): map VINTR / VQUIT / VSUSP
 *     to SIGINT / SIGQUIT / SIGTSTP and consume the byte (don't
 *     pass it upstream).
 *   - Canonical input (ICANON): buffer bytes into a per-instance
 *     line buffer until VEOL/NL/VEOF; deliver a whole line as one
 *     mblk.  Within the buffer, VERASE drops the last char and
 *     VKILL drops the whole line.
 *   - Echo (ECHO/ECHOE/ECHOK): write each accepted byte back DOWN
 *     to the driver so the user sees what they typed.  VERASE
 *     does the classic BS-SP-BS dance.
 *   - Output processing (OPOST/ONLCR): on the way down, map NL ->
 *     CR-LF so user programs that emit Unix newlines get DOS-style
 *     line endings on the tty.
 *
 * Per-instance state (struct ldterm) lives in q_ptr of both the
 * read and write queues (q_link wires them together).  Allocated
 * at qi_qopen time, freed at qi_qclose.
 *
 * What's NOT here yet:
 *   - TCGETA / TCSETA ioctls (need M_IOCTL flow through the stream
 *     stack -- audit gap #5).  For now the default termios is the
 *     only setting.
 *   - VMIN / VTIME (non-canonical mode timeouts).
 *   - IXON / IXOFF flow control.
 *   - Job control SIGTTIN / SIGTTOU (no sessions yet -- phase 5).
 */

#include <stdint.h>

#include "kappara/kmem.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/signal.h"
#include "kappara/streams.h"
#include "kappara/stream_head.h"
#include "kappara/string.h"
#include "kappara/termios.h"
#include "kappara/tty.h"

/* ---- per-instance state -------------------------------------------------- */

#define LDTERM_LINEBUF_MAX	512

struct ldterm {
	struct termios termios;
	char canon_buf[LDTERM_LINEBUF_MAX];
	int  canon_len;
};

static void ldterm_set_default_termios(struct termios *t)
{
	kmemset(t, 0, sizeof(*t));
	t->c_iflag = BRKINT | ICRNL;
	t->c_oflag = OPOST  | ONLCR;
	t->c_cflag = CS8    | CREAD | CLOCAL;
	t->c_lflag = ICANON | ECHO  | ECHOE | ECHOK | ISIG;
	t->c_cc[VINTR]  = CTRL('C');	/* 0x03 -- ^C SIGINT  */
	t->c_cc[VQUIT]  = CTRL('\\');	/* 0x1c -- ^\ SIGQUIT */
	t->c_cc[VERASE] = 0x7f;		/* DEL (a.k.a. ^?)    */
	t->c_cc[VKILL]  = CTRL('U');	/* 0x15 -- ^U         */
	t->c_cc[VEOF]   = CTRL('D');	/* 0x04 -- ^D EOF     */
	t->c_cc[VEOL]   = 0;		/* unset              */
}

/* ---- helpers ------------------------------------------------------------- */

/* Append one byte to a freshly-allocb'd mblk and send it DOWN the
 * stream (towards the driver) via the WRITE queue.  Used by echo
 * and by the OPOST output path. */
static void ldterm_putdown_byte(queue_t *rq, uint8_t b)
{
	queue_t *wq = rq->q_link;
	if (!wq) return;
	mblk_t *mp = allocb(1, 0);
	if (!mp) return;
	*mp->b_wptr++ = b;
	/* The "down" direction from the read side's perspective is
	 * the write-side queue's NEXT.  wq->q_next points at the
	 * driver (or another module below).  putnext on wq lands us
	 * one step downstream. */
	if (wq->q_next)
		putnext(wq, mp);
	else
		freemsg(mp);
}

static void ldterm_echo(queue_t *rq, struct ldterm *ld, uint8_t b)
{
	if (!(ld->termios.c_lflag & ECHO))
		return;
	/* ECHOCTL would render control chars as "^X"; we keep it
	 * simple for phase 2 -- the common case is just NL and
	 * printable bytes. */
	ldterm_putdown_byte(rq, b);
	if (b == '\n' && (ld->termios.c_oflag & ONLCR)) {
		/* When ONLCR fires on the write side it adds the CR;
		 * but the echo of a literal NL goes through THIS path,
		 * not OPOST -- so we tack the CR on here too so the
		 * user sees the cursor return.  ECHONL would force
		 * echo even when ECHO is off; we don't model it. */
		ldterm_putdown_byte(rq, '\r');
	}
}

static void ldterm_echo_erase(queue_t *rq, struct ldterm *ld)
{
	if (!(ld->termios.c_lflag & ECHO))
		return;
	if (ld->termios.c_lflag & ECHOE) {
		/* Visually erase: backspace, space (overwrites the
		 * char), backspace. */
		ldterm_putdown_byte(rq, '\b');
		ldterm_putdown_byte(rq, ' ');
		ldterm_putdown_byte(rq, '\b');
	} else {
		ldterm_putdown_byte(rq, '\b');
	}
}

/* Send the current canon_buf upstream as one mblk + reset.
 * include_terminator controls whether the line-terminator
 * (typically NL) is appended; VEOF on an empty buffer should
 * deliver zero bytes (the "EOF mblk"). */
static void ldterm_deliver_line(queue_t *rq, struct ldterm *ld,
				int append_terminator)
{
	int total = ld->canon_len + (append_terminator ? 1 : 0);
	mblk_t *mp = allocb(total > 0 ? (size_t)total : 1, 0);
	if (!mp) {
		ld->canon_len = 0;
		return;
	}
	for (int i = 0; i < ld->canon_len; i++)
		*mp->b_wptr++ = (uint8_t)ld->canon_buf[i];
	if (append_terminator)
		*mp->b_wptr++ = '\n';
	ld->canon_len = 0;
	putnext(rq, mp);
}

static void ldterm_signal_reader(queue_t *rq, unsigned sig)
{
	/* SVR4: signal goes to the controlling tty's foreground
	 * process group.  Phase 5 wires it via tty_signal_fg_pgrp,
	 * which itself falls back to sd_last_reader when no fg_pgrp
	 * has been set (the pre-session-model behaviour kept around
	 * so the existing shell still receives Ctrl-C before user
	 * code calls tcsetpgrp).
	 *
	 * ldterm assumes its stream's sd_minor is a tty minor since
	 * the only auto-push path puts it on /dev/tty<N>.  If
	 * someone hand-pushes ldterm onto /dev/loop or similar,
	 * tty_signal_fg_pgrp() with the unrelated minor would
	 * misroute -- not worth a guard for since that pattern
	 * never occurs in practice. */
	queue_t *up = rq->q_next;
	if (!up || !up->q_ptr) return;
	struct stdata *sd = (struct stdata *)up->q_ptr;
	tty_signal_fg_pgrp((int)sd->sd_minor, sig);
}

/* ---- read side: bytes from driver coming UP ------------------------------ */

static int ldterm_rq_putp(queue_t *q, mblk_t *mp)
{
	struct ldterm *ld = q->q_ptr;
	if (!ld) {
		/* Pre-qopen mblks shouldn't happen, but if they do, just
		 * pass them through to the head. */
		putnext(q, mp);
		return 0;
	}

	/* Non-M_DATA control messages (M_HANGUP, M_FLUSH, ...) are
	 * forwarded unchanged.  Only M_DATA is cooked. */
	if (mp->b_datap->db_type != M_DATA) {
		putnext(q, mp);
		return 0;
	}

	int iflag = ld->termios.c_iflag;
	int lflag = ld->termios.c_lflag;

	while (mp->b_rptr < mp->b_wptr) {
		uint8_t b = *mp->b_rptr++;

		/* ICRNL / INLCR / IGNCR -- CR/NL massaging on input. */
		if (b == '\r') {
			if (iflag & IGNCR)         continue;
			if (iflag & ICRNL)         b = '\n';
		} else if (b == '\n' && (iflag & INLCR)) {
			b = '\r';
		}

		/* ISIG: VINTR / VQUIT generate signals and consume the
		 * byte.  Do this BEFORE ICANON so they fire even when
		 * the line buffer has pending data. */
		if (lflag & ISIG) {
			if (b == ld->termios.c_cc[VINTR]) {
				/* Optional: flush the line buf unless
				 * NOFLSH.  Without sessions, just drop. */
				if (!(lflag & NOFLSH))
					ld->canon_len = 0;
				ldterm_signal_reader(q, SIGINT);
				continue;
			}
			if (b == ld->termios.c_cc[VQUIT]) {
				if (!(lflag & NOFLSH))
					ld->canon_len = 0;
				ldterm_signal_reader(q, SIGQUIT);
				continue;
			}
		}

		if (!(lflag & ICANON)) {
			/* Raw mode: echo if enabled, deliver one byte at
			 * a time straight up.  No buffering, no line
			 * editing. */
			if (lflag & ECHO) ldterm_echo(q, ld, b);
			mblk_t *up = allocb(1, 0);
			if (up) {
				*up->b_wptr++ = b;
				putnext(q, up);
			}
			continue;
		}

		/* ---- canonical mode ---- */

		if (b == ld->termios.c_cc[VERASE]) {
			if (ld->canon_len > 0) {
				ld->canon_len--;
				ldterm_echo_erase(q, ld);
			}
			continue;
		}
		if (b == ld->termios.c_cc[VKILL]) {
			while (ld->canon_len > 0) {
				ld->canon_len--;
				ldterm_echo_erase(q, ld);
			}
			if (lflag & ECHOK) ldterm_echo(q, ld, '\n');
			continue;
		}
		if (b == ld->termios.c_cc[VEOF]) {
			/* VEOF: deliver whatever is buffered immediately
			 * (no terminator); on an empty buffer this is a
			 * 0-byte mblk = EOF marker for the reader. */
			ldterm_deliver_line(q, ld, 0);
			continue;
		}
		if (b == '\n' ||
		    (ld->termios.c_cc[VEOL] != 0 &&
		     b == ld->termios.c_cc[VEOL])) {
			ldterm_echo(q, ld, b);
			ldterm_deliver_line(q, ld, 1);
			continue;
		}

		/* Plain byte -- echo + append to line buf.  If the buffer
		 * is full, silently drop (real ldterm beeps; we don't have
		 * a BEL output path yet). */
		ldterm_echo(q, ld, b);
		if (ld->canon_len < LDTERM_LINEBUF_MAX)
			ld->canon_buf[ld->canon_len++] = (char)b;
	}

	freemsg(mp);
	return 0;
}

/* ---- write side: bytes from head going DOWN ----------------------------- */

static int ldterm_wq_putp(queue_t *q, mblk_t *mp)
{
	struct ldterm *ld = q->q_ptr;
	if (!ld || mp->b_datap->db_type != M_DATA) {
		putnext(q, mp);
		return 0;
	}

	if (!(ld->termios.c_oflag & OPOST)) {
		/* No output processing -- pass through. */
		putnext(q, mp);
		return 0;
	}

	/* OPOST: walk the mblk, expand NL -> CR-LF when ONLCR is set,
	 * map CR -> NL when OCRNL is set.  Build a new mblk so we
	 * don't have to grow the original in place.  This is the
	 * straight-line implementation; a production ldterm would
	 * avoid the copy when no translation is needed. */
	size_t need = 0;
	for (uint8_t *p = mp->b_rptr; p < mp->b_wptr; p++) {
		if (*p == '\n' && (ld->termios.c_oflag & ONLCR))
			need += 2;
		else
			need += 1;
	}
	mblk_t *out = allocb(need ? need : 1, 0);
	if (!out) {
		freemsg(mp);
		return 0;
	}
	for (uint8_t *p = mp->b_rptr; p < mp->b_wptr; p++) {
		uint8_t b = *p;
		if (b == '\n' && (ld->termios.c_oflag & ONLCR)) {
			*out->b_wptr++ = '\r';
			*out->b_wptr++ = '\n';
		} else if (b == '\r' && (ld->termios.c_oflag & OCRNL)) {
			*out->b_wptr++ = '\n';
		} else {
			*out->b_wptr++ = b;
		}
	}
	freemsg(mp);
	putnext(q, out);
	return 0;
}

/* ---- qopen / qclose ----------------------------------------------------- */

static int ldterm_qopen(queue_t *q)
{
	struct ldterm *ld = kmalloc(sizeof(*ld));
	if (!ld) {
		kprintf("ldterm: kmalloc failed at qopen\n");
		return -1;
	}
	kmemset(ld, 0, sizeof(*ld));
	ldterm_set_default_termios(&ld->termios);
	q->q_ptr = ld;
	if (q->q_link)
		q->q_link->q_ptr = ld;
	return 0;
}

static int ldterm_qclose(queue_t *q)
{
	if (q->q_ptr) {
		kfree(q->q_ptr);
		q->q_ptr = 0;
		if (q->q_link) q->q_link->q_ptr = 0;
	}
	return 0;
}

/* ---- registration ------------------------------------------------------- */

static struct module_info ldterm_minfo = {
	.mi_idnum  = 200,
	.mi_idname = "ldterm",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit ldterm_rinit = {
	.qi_putp   = ldterm_rq_putp,
	.qi_qopen  = ldterm_qopen,
	.qi_qclose = ldterm_qclose,
	.qi_minfo  = &ldterm_minfo,
};

static struct qinit ldterm_winit = {
	.qi_putp  = ldterm_wq_putp,
	.qi_minfo = &ldterm_minfo,
};

static struct streamtab ldterm_streamtab = {
	.st_rdinit = &ldterm_rinit,
	.st_wrinit = &ldterm_winit,
};

void ldterm_init(void)
{
	streams_register("ldterm", &ldterm_streamtab);
}

/* ---- self-test --------------------------------------------------------- */

/* Static accumulator -- bytes the synthetic "head" / "driver" see
 * during the selftest land here so we can assert on them without
 * round-tripping through real mblks.  Cleared between cases. */
struct ldterm_st_sink {
	uint8_t buf[256];
	int     len;
};
static struct ldterm_st_sink ldterm_st_up_sink;   /* what the head saw */
static struct ldterm_st_sink ldterm_st_down_sink; /* what the driver saw */

static int ldterm_st_putp_up(queue_t *q, mblk_t *mp)
{
	(void)q;
	for (uint8_t *p = mp->b_rptr; p < mp->b_wptr; p++)
		if (ldterm_st_up_sink.len < (int)sizeof(ldterm_st_up_sink.buf))
			ldterm_st_up_sink.buf[ldterm_st_up_sink.len++] = *p;
	freemsg(mp);
	return 0;
}

static int ldterm_st_putp_down(queue_t *q, mblk_t *mp)
{
	(void)q;
	for (uint8_t *p = mp->b_rptr; p < mp->b_wptr; p++)
		if (ldterm_st_down_sink.len < (int)sizeof(ldterm_st_down_sink.buf))
			ldterm_st_down_sink.buf[ldterm_st_down_sink.len++] = *p;
	freemsg(mp);
	return 0;
}

static struct module_info st_dummy_minfo = {
	.mi_idnum = 999, .mi_idname = "ldterm_st",
	.mi_minpsz = 0, .mi_maxpsz = 4096,
	.mi_hiwat = 16384, .mi_lowat = 8192,
};

static struct qinit st_up_qinit_r = {
	.qi_putp = ldterm_st_putp_up, .qi_minfo = &st_dummy_minfo,
};
static struct qinit st_up_qinit_w = {
	.qi_putp = ldterm_st_putp_up, .qi_minfo = &st_dummy_minfo,
};
static struct qinit st_down_qinit_r = {
	.qi_putp = ldterm_st_putp_down, .qi_minfo = &st_dummy_minfo,
};
static struct qinit st_down_qinit_w = {
	.qi_putp = ldterm_st_putp_down, .qi_minfo = &st_dummy_minfo,
};

/* Feed `bytes` into the read side via an mblk; the result goes UP into
 * up_sink (deliver_line, raw passthrough) or DOWN into down_sink
 * (echo, OPOST). */
static void ldterm_st_feed(queue_t *ldterm_rq, const char *bytes, int n)
{
	mblk_t *mp = allocb((size_t)n, 0);
	if (!mp) return;
	for (int i = 0; i < n; i++) *mp->b_wptr++ = (uint8_t)bytes[i];
	ldterm_rq_putp(ldterm_rq, mp);
}

void ldterm_selftest(void)
{
	/* Build a tiny stream: synthetic head (up) --- ldterm --- synthetic
	 * driver (down).  ldterm's rq->q_next = head's rq; ldterm's
	 * wq->q_next = driver's wq.  q_link inside ldterm pairs the two. */
	queue_t head_rq, head_wq;
	queue_t ld_rq, ld_wq;
	queue_t drv_rq, drv_wq;
	queue_init_pair(&head_rq, &head_wq, &st_up_qinit_r,   &st_up_qinit_w);
	queue_init_pair(&ld_rq,   &ld_wq,   &ldterm_rinit,    &ldterm_winit);
	queue_init_pair(&drv_rq,  &drv_wq,  &st_down_qinit_r, &st_down_qinit_w);

	/* Wire neighbours: upstream of ldterm's rq is head_rq; upstream
	 * of ldterm's wq is drv_wq (the "down" direction from head's
	 * POV is going further away from the user). */
	ld_rq.q_next = &head_rq;
	ld_wq.q_next = &drv_wq;

	if (ldterm_qopen(&ld_rq) != 0) {
		kprintf("ldterm: SELFTEST FAIL qopen\n");
		return;
	}

	int ok = 1;

	/* (1) Canonical mode: type "hi\n"; head should get "hi\n",
	 *     driver should see the echo of all three bytes (h, i,
	 *     and the NL with CR appended). */
	ldterm_st_up_sink.len = 0;
	ldterm_st_down_sink.len = 0;
	ldterm_st_feed(&ld_rq, "hi\n", 3);
	if (ldterm_st_up_sink.len != 3
	    || ldterm_st_up_sink.buf[0] != 'h'
	    || ldterm_st_up_sink.buf[1] != 'i'
	    || ldterm_st_up_sink.buf[2] != '\n') {
		kprintf("ldterm: SELFTEST FAIL canon-line len=%d\n",
			ldterm_st_up_sink.len);
		ok = 0;
	}
	/* Echo: h, i, \n, \r (ECHO + ONLCR-on-echo). */
	if (ldterm_st_down_sink.len != 4
	    || ldterm_st_down_sink.buf[0] != 'h'
	    || ldterm_st_down_sink.buf[1] != 'i'
	    || ldterm_st_down_sink.buf[2] != '\n'
	    || ldterm_st_down_sink.buf[3] != '\r') {
		kprintf("ldterm: SELFTEST FAIL canon-echo len=%d\n",
			ldterm_st_down_sink.len);
		ok = 0;
	}

	/* (2) Erase: type "ax", DEL, "\n" -> head gets "a\n", driver
	 *     sees echo 'a','x', then BS-SP-BS for erase, then '\n','\r'. */
	ldterm_st_up_sink.len = 0;
	ldterm_st_down_sink.len = 0;
	ldterm_st_feed(&ld_rq, "ax\x7f\n", 4);
	if (ldterm_st_up_sink.len != 2
	    || ldterm_st_up_sink.buf[0] != 'a'
	    || ldterm_st_up_sink.buf[1] != '\n') {
		kprintf("ldterm: SELFTEST FAIL erase-line len=%d\n",
			ldterm_st_up_sink.len);
		ok = 0;
	}

	/* (3) VINTR (^C) in ICANON: shouldn't propagate upstream,
	 *     and shouldn't deliver the partial line (NOFLSH off). */
	ldterm_st_up_sink.len = 0;
	ldterm_st_down_sink.len = 0;
	ldterm_st_feed(&ld_rq, "partial\x03", 8);
	if (ldterm_st_up_sink.len != 0) {
		kprintf("ldterm: SELFTEST FAIL intr-consumed got=%d\n",
			ldterm_st_up_sink.len);
		ok = 0;
	}

	/* (4) OPOST + ONLCR on the write side: "hi\n" -> "hi\r\n"
	 *     down to the driver. */
	ldterm_st_up_sink.len = 0;
	ldterm_st_down_sink.len = 0;
	{
		mblk_t *out = allocb(3, 0);
		*out->b_wptr++ = 'h'; *out->b_wptr++ = 'i'; *out->b_wptr++ = '\n';
		ldterm_wq_putp(&ld_wq, out);
	}
	if (ldterm_st_down_sink.len != 4
	    || ldterm_st_down_sink.buf[0] != 'h'
	    || ldterm_st_down_sink.buf[1] != 'i'
	    || ldterm_st_down_sink.buf[2] != '\r'
	    || ldterm_st_down_sink.buf[3] != '\n') {
		kprintf("ldterm: SELFTEST FAIL opost len=%d\n",
			ldterm_st_down_sink.len);
		ok = 0;
	}

	ldterm_qclose(&ld_rq);

	if (ok)
		kprintf("ldterm: selftest PASS\n");
}

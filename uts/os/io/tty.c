/*
 * kernel/tty.c -- multi-minor TTY (virtual console) driver
 *
 * Phase 3 of the virtual-console roadmap.  See kappara/io/tty.h for
 * the big picture and what's deferred.
 *
 * Stream stack shape (per /dev/ttyN):
 *
 *     stream_head  --  (ldterm, eventually)  --  tty driver
 *                                                ^^^^^^^^^^^
 *     write path: head_wq.putp -> putnext -> drv_wq.putp = tty_drv_wq_putp
 *     read path:  driver pushes bytes via putnext on drv_rq;
 *                 head_rq.putp queues them for sys_read.
 *
 * The interesting bit is the write side: tty_drv_wq_putp walks the
 * mblk, feeds each byte through the per-minor VT100 emulator's
 * `vt_feed`, and additionally writes the byte straight to UART if
 * this minor is the currently active one.  Inactive minors update
 * only their cell buffer; the user sees nothing until
 * `tty_switch(minor)` clears the host terminal and replays the cell
 * buffer as ANSI.
 *
 * The read side isn't wired yet -- nothing pushes bytes UP into the
 * stream because UART rx still feeds `console_active` directly.
 * Phase 6 (multi-shell spawn) is where that becomes a real path.
 */

#include <stdint.h>

#include "kappara/io/cdevsw.h"
#include "kappara/io/fbcon.h"
#include "kappara/core/kmem.h"
#include "kappara/core/printk.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/core/string.h"
#include "kappara/io/tty.h"
#include "kappara/arch/uart.h"
#include "kappara/io/vt.h"

static struct tty_minor tty_minors[NTTY];
static int              tty_active_minor;
static unsigned         tty_fg_pgrp_arr[NTTY];

int tty_active(void)
{
	return tty_active_minor;
}

struct queue *tty_drv_rq(int minor)
{
	if (minor < 0 || minor >= NTTY) return 0;
	struct tty_minor *m = &tty_minors[minor];
	if (!m->sd) return 0;
	return m->sd->sd_drv_rq;
}

void tty_set_fg_pgrp(int minor, unsigned pgrp)
{
	if (minor < 0 || minor >= NTTY) return;
	tty_fg_pgrp_arr[minor] = pgrp;
}

unsigned tty_fg_pgrp(int minor)
{
	if (minor < 0 || minor >= NTTY) return 0;
	return tty_fg_pgrp_arr[minor];
}

int tty_signal_fg_pgrp(int minor, unsigned sig)
{
	if (minor < 0 || minor >= NTTY) return 0;
	unsigned pg = tty_fg_pgrp_arr[minor];
	if (pg != 0)
		return kthread_signal_pgrp(pg, sig);
	/* Fallback to sd_last_reader: same shape uart_rx_main has used
	 * since before sessions existed.  Lets the existing pre-Phase-5
	 * shell flow keep working until tcsetpgrp is wired in user
	 * space. */
	struct tty_minor *m = &tty_minors[minor];
	if (!m->sd) return 0;
	if (!m->sd->sd_last_reader) return 0;
	struct kthread *t = kthread_find(m->sd->sd_last_reader);
	if (!t) return 0;
	return kthread_signal(t, sig) == 0 ? 1 : 0;
}

/* ---- driver qinit ----------------------------------------------------- */

/* qi_qopen: stream_build has wired sd as drv_rq->q_ptr as the
 * minor-lookup backref.  Replace it with our per-minor state
 * pointer so all subsequent qi_putp calls (which see the same q
 * struct) read straight from there. */
static int tty_drv_qopen(queue_t *q)
{
	struct stdata *sd = q->q_ptr;
	if (!sd) {
		kprintf("tty: qopen with no sd backref\n");
		return -1;
	}
	unsigned minor = sd->sd_minor;
	if (minor >= NTTY) {
		kprintf("tty: qopen with OOB minor %u\n", minor);
		return -1;
	}
	struct tty_minor *m = &tty_minors[minor];
	m->sd = sd;
	q->q_ptr           = m;
	if (q->q_link)
		q->q_link->q_ptr = m;	/* write side shares state */
	return 0;
}

static int tty_drv_qclose(queue_t *q)
{
	struct tty_minor *m = q->q_ptr;
	if (m) {
		/* Don't free m -- the array is static.  Just clear the
		 * stdata backref so a subsequent close-then-reopen
		 * binds a fresh sd cleanly. */
		m->sd = 0;
	}
	q->q_ptr = 0;
	if (q->q_link) q->q_link->q_ptr = 0;
	return 0;
}

/* Bytes coming DOWN from the head (sys_write payload) land here.
 * Feed each byte through the per-minor VT emulator's cell buffer;
 * if this minor is the currently-active tty, also send the byte
 * straight to UART so the user sees output live. */
static int tty_drv_wq_putp(queue_t *q, mblk_t *mp)
{
	struct tty_minor *m = q->q_ptr;
	if (!m) {
		freemsg(mp);
		return 0;
	}
	/* M_IOCTL that fell through the stack with nobody recognising
	 * it -- we're the bottom, NAK it back upstream so the head's
	 * strioctl returns -1 instead of waiting forever. */
	if (mp->b_datap->db_type == M_IOCTL) {
		mp->b_datap->db_type = M_IOCNAK;
		struct iocblk *ic = (struct iocblk *)mp->b_rptr;
		ic->ic_error = -1;
		ic->ic_count = 0;
		if (mp->b_cont) {
			freemsg(mp->b_cont);
			mp->b_cont = NULL;
		}
		queue_t *rq = q->q_link;
		if (rq) putnext(rq, mp);
		else    freemsg(mp);
		return 0;
	}
	if (mp->b_datap->db_type != M_DATA) {
		freemsg(mp);
		return 0;
	}

	int active = (m->minor == tty_active_minor);

	/* Remote-tty output redirect (telnetd etc).  If a hook is
	 * installed, every byte goes through it instead of touching
	 * the UART.  The vt cell buffer still updates so a follow-up
	 * tty_switch can replay history. */
	if (m->output_hook) {
		for (uint8_t *p = mp->b_rptr; p < mp->b_wptr; p++)
			vt_feed(&m->vt, *p);
		m->output_hook(m->minor, mp->b_rptr,
		               (unsigned)(mp->b_wptr - mp->b_rptr),
		               m->output_hook_arg);
	} else if (active) {
		/* Batch the UART acquire across the whole mblk -- one
		 * lock acquire instead of one per byte. */
		unsigned long flags = uart_acquire();
		for (uint8_t *p = mp->b_rptr; p < mp->b_wptr; p++) {
			vt_feed(&m->vt, *p);
			uart_putc_unlocked((char)*p);
		}
		uart_release(flags);
		/* Phase 7: incremental fb repaint after the mblk.  Once
		 * per mblk rather than once per byte -- the shell tends
		 * to write a whole prompt/echo line as a single mblk, so
		 * this is much cheaper than per-byte yet still feels
		 * live to the user.  No-op when no framebuffer is up. */
		fbcon_render_vt(&m->vt);
	} else {
		/* Inactive: only update the cell buffer.  Bytes will
		 * surface on the next tty_switch to this minor. */
		for (uint8_t *p = mp->b_rptr; p < mp->b_wptr; p++)
			vt_feed(&m->vt, *p);
	}

	freemsg(mp);
	return 0;
}

/* Read-side putp.  Driver-up traffic (bytes the kernel injected via
 * putnext on drv_rq) goes upstream to the head.  Phase 3 doesn't
 * use this -- nothing pushes UP into a /dev/tty<N> yet -- but the
 * shape stays in place so phase 6 can hook it. */
static int tty_drv_rq_putp(queue_t *q, mblk_t *mp)
{
	putnext(q, mp);
	return 0;
}

static struct module_info tty_drv_minfo = {
	.mi_idnum  = 300,
	.mi_idname = "tty",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit tty_drv_rinit = {
	.qi_putp   = tty_drv_rq_putp,
	.qi_qopen  = tty_drv_qopen,
	.qi_qclose = tty_drv_qclose,
	.qi_minfo  = &tty_drv_minfo,
};

static struct qinit tty_drv_winit = {
	.qi_putp  = tty_drv_wq_putp,
	.qi_minfo = &tty_drv_minfo,
};

static struct streamtab tty_streamtab = {
	.st_rdinit = &tty_drv_rinit,
	.st_wrinit = &tty_drv_winit,
};

/* ---- switch + repaint ------------------------------------------------ */

/* Emit a CSI for the given two-digit numeric param.  Bytes go via
 * uart_putc_unlocked so the caller's uart_acquire scope batches
 * cleanly. */
static void tty_emit_dec(int v)
{
	if (v >= 100) {
		uart_putc_unlocked((char)('0' + (v / 100)));
		v %= 100;
		uart_putc_unlocked((char)('0' + (v / 10)));
		uart_putc_unlocked((char)('0' + (v % 10)));
	} else if (v >= 10) {
		uart_putc_unlocked((char)('0' + (v / 10)));
		uart_putc_unlocked((char)('0' + (v % 10)));
	} else {
		uart_putc_unlocked((char)('0' + v));
	}
}

static void tty_emit_str(const char *s)
{
	while (*s) uart_putc_unlocked(*s++);
}

/* Convert one VT_COLOR_* to its SGR fg parameter (30-37 / 90-97). */
static int tty_sgr_fg(uint8_t c)
{
	if (c == VT_COLOR_DEFAULT) return 39;
	if (c & VT_COLOR_BRIGHT)   return 90 + (c & 0x7);
	return 30 + (c & 0x7);
}

static int tty_sgr_bg(uint8_t c)
{
	if (c == VT_COLOR_DEFAULT) return 49;
	if (c & VT_COLOR_BRIGHT)   return 100 + (c & 0x7);
	return 40 + (c & 0x7);
}

/* Replay v's cell buffer as ANSI over UART.  Compresses by emitting
 * SGR only when (fg, bg, flags) change between adjacent cells, and
 * by trimming trailing blanks from each row.  Final emit positions
 * the cursor at the VT's current (row, col). */
static void tty_repaint(const struct vt *v)
{
	unsigned long flags = uart_acquire();

	/* Clear screen, home cursor. */
	tty_emit_str("\x1b[2J\x1b[H");
	/* Force a SGR reset so the prev attr cache below kicks off
	 * with known state. */
	tty_emit_str("\x1b[0m");
	int  cur_fg    = -1;
	int  cur_bg    = -1;
	uint8_t cur_fl = 0xff;

	for (int r = 0; r < VT_ROWS; r++) {
		int last_nonblank = -1;
		for (int c = 0; c < VT_COLS; c++) {
			const struct vt_cell *cell = &v->cells[r][c];
			if (cell->ch != ' ' || cell->bg != VT_COLOR_DEFAULT)
				last_nonblank = c;
		}
		if (last_nonblank < 0)
			continue;	/* whole row blank */

		/* Position to (r+1, 1) -- 1-based for ANSI. */
		tty_emit_str("\x1b[");
		tty_emit_dec(r + 1);
		uart_putc_unlocked(';');
		uart_putc_unlocked('1');
		uart_putc_unlocked('H');

		for (int c = 0; c <= last_nonblank; c++) {
			const struct vt_cell *cell = &v->cells[r][c];
			int fg = tty_sgr_fg(cell->fg);
			int bg = tty_sgr_bg(cell->bg);
			if (fg != cur_fg || bg != cur_bg
			               || cell->flags != cur_fl) {
				tty_emit_str("\x1b[0");
				if (cell->flags & VT_FLAG_BOLD)
					tty_emit_str(";1");
				if (cell->flags & VT_FLAG_UNDERLINE)
					tty_emit_str(";4");
				if (cell->flags & VT_FLAG_REVERSE)
					tty_emit_str(";7");
				uart_putc_unlocked(';');
				tty_emit_dec(fg);
				uart_putc_unlocked(';');
				tty_emit_dec(bg);
				uart_putc_unlocked('m');
				cur_fg = fg;
				cur_bg = bg;
				cur_fl = cell->flags;
			}
			uart_putc_unlocked((char)cell->ch);
		}
	}

	/* Land the cursor at the VT's current position. */
	tty_emit_str("\x1b[");
	tty_emit_dec(v->row + 1);
	uart_putc_unlocked(';');
	tty_emit_dec(v->col + 1);
	uart_putc_unlocked('H');

	uart_release(flags);
}

void tty_switch(int i)
{
	if (i < 0 || i >= NTTY) return;
	if (i == tty_active_minor) return;
	tty_active_minor = i;
	tty_repaint(&tty_minors[i].vt);
	/* Force a full fb repaint -- the dirty bitmap only tracks
	 * changes SINCE THE LAST PAINT, but the framebuffer is showing
	 * whatever the previous tty left there, so we need to paint
	 * the entire grid.  No-op when no framebuffer is initialised
	 * (QEMU -display none keeps working transparently). */
	vt_mark_all_dirty(&tty_minors[i].vt);
	fbcon_render_vt(&tty_minors[i].vt);
}

/* ---- remote-tty hooks (bridge to telnetd etc) --------------------------- */

void tty_set_output_hook(int minor,
                         void (*fn)(int, const void *, unsigned, void *),
                         void *arg)
{
	if (minor < 0 || minor >= NTTY) return;
	tty_minors[minor].output_hook     = fn;
	tty_minors[minor].output_hook_arg = arg;
}

void tty_remote_feed_byte(int minor, unsigned char c)
{
	if (minor < 0 || minor >= NTTY) return;
	queue_t *rq = tty_drv_rq(minor);
	if (!rq || !rq->q_next) return;
	mblk_t *mp = allocb(1, 0);
	if (!mp) return;
	mp->b_datap->db_type = M_DATA;
	*mp->b_wptr++ = c;
	putnext(rq, mp);
}

/* ---- init ------------------------------------------------------------- */

void tty_init(void)
{
	for (int i = 0; i < NTTY; i++) {
		vt_init(&tty_minors[i].vt);
		tty_minors[i].sd    = 0;
		tty_minors[i].minor = i;
	}
	tty_active_minor = 0;

	streams_register("tty",  &tty_streamtab);
	cdev_register(CDEV_MAJ_TTY, "tty", &tty_streamtab);
	/* /dev/tty0..tty<NTTY-1> node creation lives in
	 * streams_head_init right after the /dev mkdir so we don't
	 * have to redo the lookup ourselves. */
}

/* ---- self-test ------------------------------------------------------- */

void tty_selftest(void)
{
	int ok = 1;

	/* Drive vt_feed directly via the per-minor buffer -- no need
	 * to build an open stream just to exercise the cell update +
	 * switch repaint logic. */
	struct tty_minor *m1 = &tty_minors[1];
	vt_init(&m1->vt);
	const uint8_t hello[] = "Hi\r\n";
	for (size_t i = 0; i < sizeof(hello) - 1; i++)
		vt_feed(&m1->vt, hello[i]);

	const struct vt_cell *c00 = vt_cell_at(&m1->vt, 0, 0);
	const struct vt_cell *c01 = vt_cell_at(&m1->vt, 0, 1);
	if (!c00 || c00->ch != 'H' || !c01 || c01->ch != 'i') {
		kprintf("tty: SELFTEST FAIL cell-feed\n");
		ok = 0;
	}
	if (m1->vt.row != 1 || m1->vt.col != 0) {
		kprintf("tty: SELFTEST FAIL cursor r=%d c=%d\n",
			m1->vt.row, m1->vt.col);
		ok = 0;
	}

	/* Switch should not crash and should set the active minor. */
	int prev = tty_active_minor;
	tty_switch(1);
	if (tty_active_minor != 1) {
		kprintf("tty: SELFTEST FAIL switch active=%d\n",
			tty_active_minor);
		ok = 0;
	}
	tty_switch(prev);
	if (tty_active_minor != prev) {
		kprintf("tty: SELFTEST FAIL switch-back active=%d\n",
			tty_active_minor);
		ok = 0;
	}

	if (ok)
		kprintf("tty: selftest PASS\n");
}

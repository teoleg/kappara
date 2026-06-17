/*
 * uts/virt/telnetd.c -- in-kernel telnet listener bridged to tty4
 *
 * On accept:
 *   1. Drop a per-session output hook on tty4 -- every byte the shell
 *      writes to tty4 (its stdout) gets pushed into the TCP socket.
 *   2. Spin a read loop: TCP bytes -> tty_remote_feed_byte(4, b), so
 *      the shell on tty4 sees keystrokes exactly like ldterm + the
 *      head's putq would deliver them from UART.
 *   3. On EOF / orderly close: clear the hook, ORDREL the TCP side.
 *
 * The user-mode shell on tty4 is just one of the NTTY user-inits
 * spawned at boot.  It blocks in sys_read on /dev/tty4 until telnetd
 * starts pumping bytes; when the session ends, the shell goes back
 * to sleep waiting for the next connection.
 *
 * Only one telnet client at a time -- a second accept while the
 * first is live would race on the tty4 hook.  Phase 6 would add a
 * per-connection pty-pair so concurrent sessions get independent
 * shells; for phase 5.5 we just keep the listener single-shot
 * between accepts.
 */

#include <stdint.h>

#include "kappara/core/kmem.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"
#include "kappara/io/streams.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/tty.h"
#include "kappara/net/ip.h"
#include "kappara/net/tcp.h"
#include "kappara/proc/sched.h"

#define TELNET_PORT		23
#define TELNET_REMOTE_TTY	4

#define TELNET_BANNER \
	"\r\nWelcome to kappara via telnet.  Real shell on tty4.\r\n" \
	"Type a command and press Enter -- ls, cat /proc/tcp, ps, etc.\r\n\r\n"

/* ---- TPI helpers ------------------------------------------------ */

static int put_prim(struct stdata *sd, const void *body, unsigned blen)
{
	mblk_t *mp = allocb(blen, 0);
	if (!mp) return -1;
	mp->b_datap->db_type = M_PROTO;
	kmemcpy(mp->b_wptr, body, blen);
	mp->b_wptr += blen;
	putnext(sd->sd_wq, mp);
	return 0;
}

static mblk_t *wait_next_mblk(struct stdata *sd)
{
	unsigned long flags =
	    spin_lock_irq_save(&sd->sd_readwait.sq_lock);
	for (;;) {
		mblk_t *mp = getq(sd->sd_rq);
		if (mp) {
			spin_unlock_irq_restore(&sd->sd_readwait.sq_lock,
			                        flags);
			return mp;
		}
		if (sd->sd_flags & SD_EOF) {
			spin_unlock_irq_restore(&sd->sd_readwait.sq_lock,
			                        flags);
			return 0;
		}
		kthread_sleep_on_locked(&sd->sd_readwait, flags);
		flags = spin_lock_irq_save(&sd->sd_readwait.sq_lock);
	}
}

static int wait_prim(struct stdata *sd, uint8_t want)
{
	for (;;) {
		mblk_t *mp = wait_next_mblk(sd);
		if (!mp) return -1;
		uint8_t prim = (mp->b_wptr - mp->b_rptr) >= 1
		             ? mp->b_rptr[0] : 0;
		if (prim == want) { freemsg(mp); return 0; }
		if (prim == T_TCP_DISCON_IND) { freemsg(mp); return -1; }
		freemsg(mp);
	}
}

/* Wait for T_TCP_DATA_IND; copy bytes into buf.  Returns >0 bytes,
 * 0 if peer ORDREL'd (graceful close), -1 if discon. */
static int wait_data(struct stdata *sd, char *buf, unsigned cap)
{
	for (;;) {
		mblk_t *mp = wait_next_mblk(sd);
		if (!mp) return -1;
		uint8_t prim = (mp->b_wptr - mp->b_rptr) >= 1
		             ? mp->b_rptr[0] : 0;
		if (prim == T_TCP_DATA_IND && mp->b_cont) {
			mblk_t *d = mp->b_cont;
			unsigned n = (unsigned)(d->b_wptr - d->b_rptr);
			if (n > cap) n = cap;
			kmemcpy(buf, d->b_rptr, n);
			freemsg(mp);
			return (int)n;
		}
		if (prim == T_TCP_ORDREL_IND) { freemsg(mp); return 0; }
		if (prim == T_TCP_DISCON_IND) { freemsg(mp); return -1; }
		freemsg(mp);
	}
}

static int send_data(struct stdata *sd, const char *buf, unsigned blen)
{
	mblk_t *ctl = allocb(sizeof(struct t_tcp_data_req), 0);
	if (!ctl) return -1;
	ctl->b_datap->db_type = M_PROTO;
	struct t_tcp_data_req *r = (struct t_tcp_data_req *)ctl->b_wptr;
	r->prim    = T_TCP_DATA_REQ;
	r->_pad[0] = r->_pad[1] = r->_pad[2] = 0;
	ctl->b_wptr += sizeof(*r);

	mblk_t *data = allocb(blen, 0);
	if (!data) { freemsg(ctl); return -1; }
	kmemcpy(data->b_wptr, buf, blen);
	data->b_wptr += blen;
	ctl->b_cont = data;

	putnext(sd->sd_wq, ctl);
	return 0;
}

/* ---- tty4 -> TCP output hook ----------------------------------- */

/* Called from tty_drv_wq_putp when tty4 receives output (the shell
 * writing its prompt, command output, echo of typed bytes, ...).
 * `arg` is the TCP session's stdata pointer.  Drop the bytes into
 * TCP's send buffer; flow control / pacing is TCP's problem. */
static void tty4_to_tcp(int minor, const void *buf, unsigned len, void *arg)
{
	(void)minor;
	struct stdata *tcp_sd = arg;
	if (!tcp_sd || len == 0) return;
	send_data(tcp_sd, buf, len);
}

/* ---- Per-connection session ------------------------------------ */

static void telnet_session(struct stdata *R)
{
	send_data(R, TELNET_BANNER, kstrlen(TELNET_BANNER));

	/* Install the output bridge.  From now until we clear the hook,
	 * every byte tty4 writes lands in R's TCP send buffer. */
	tty_set_output_hook(TELNET_REMOTE_TTY, tty4_to_tcp, R);

	/* Nudge the shell on tty4 so it re-prompts.  A single CR makes
	 * read_line return an empty line; the shell's dispatch is a
	 * no-op on empty input and it re-prints "kappara:/# ". */
	tty_remote_feed_byte(TELNET_REMOTE_TTY, '\r');

	for (;;) {
		char buf[64];
		int n = wait_data(R, buf, sizeof(buf));
		if (n <= 0) break;
		for (int i = 0; i < n; i++)
			tty_remote_feed_byte(TELNET_REMOTE_TTY,
			                     (unsigned char)buf[i]);
	}

	tty_set_output_hook(TELNET_REMOTE_TTY, 0, 0);
}

/* ---- Listener kthread ------------------------------------------ */

static void telnetd_main(void *arg)
{
	(void)arg;
	struct stdata *L =
	    stream_build_kernel(&ip_streamtab, "telnet_listen", 0);
	if (!L) { kprintf("telnetd: build listen failed\n"); return; }
	if (stream_push_kernel(L, "tcp") < 0) {
		kprintf("telnetd: push tcp failed\n");
		stream_destroy_kernel(L);
		return;
	}

	struct t_tcp_bind_req br = { .prim = T_TCP_BIND_REQ, .port = TELNET_PORT };
	if (put_prim(L, &br, sizeof(br)) < 0
	 || wait_prim(L, T_TCP_BIND_ACK) < 0) {
		kprintf("telnetd: bind port %d failed\n", TELNET_PORT);
		stream_destroy_kernel(L);
		return;
	}
	struct t_tcp_listen_req lr = { .prim = T_TCP_LISTEN_REQ };
	if (put_prim(L, &lr, sizeof(lr)) < 0) {
		kprintf("telnetd: listen failed\n");
		stream_destroy_kernel(L);
		return;
	}
	kprintf("telnetd: listening on TCP port %d -> shell on tty4\n",
		TELNET_PORT);

	for (;;) {
		if (wait_prim(L, T_TCP_CONN_IND) < 0) continue;
		kprintf("telnetd: incoming connection\n");

		struct stdata *R =
		    stream_build_kernel(&ip_streamtab, "telnet_resp", 0);
		if (!R || stream_push_kernel(R, "tcp") < 0) {
			kprintf("telnetd: failed to build responder\n");
			if (R) stream_destroy_kernel(R);
			continue;
		}

		if (tcp_kaccept(L, R) < 0) {
			kprintf("telnetd: tcp_kaccept failed\n");
			stream_destroy_kernel(R);
			continue;
		}

		if (wait_prim(R, T_TCP_CONN_CON) < 0) {
			stream_destroy_kernel(R);
			continue;
		}
		telnet_session(R);

		/* Graceful close. */
		struct t_tcp_ordrel_req oq = { .prim = T_TCP_ORDREL_REQ };
		put_prim(R, &oq, sizeof(oq));
		(void)wait_prim(R, T_TCP_ORDREL_IND);
		stream_destroy_kernel(R);

		kprintf("telnetd: session ended\n");
	}
}

void telnetd_init(void)
{
	kthread_create("telnetd", telnetd_main, 0);
}

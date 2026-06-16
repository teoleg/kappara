/*
 * uts/virt/telnetd.c -- in-kernel telnet listener (phase 5)
 *
 * Goal: external hosts can `telnet <qemu_host_ip> 2323` and reach a
 * kappara prompt.  QEMU's user-mode networking does the port-forward
 * via `-netdev user,hostfwd=tcp::2323-:23` (already in Makefile);
 * kappara binds TCP port 23 inside, this kthread accepts incoming
 * connections and runs a tiny command interpreter over them.
 *
 * Phase 5 scope: prove the end-to-end loop is solid.  Each connection
 * gets a banner, an echo-with-line-discipline read/write loop, and a
 * small command set (help / ps / ifconfig / netstat / quit) that
 * dumps kernel-side info straight to the TCP socket.  Full shell
 * bridging (so users run /usr/bin/<cmd> through telnet) is a follow-up
 * once the pty-pair / SVR4 streams pipe-over-tcp shape is in place.
 *
 * Wire-up: telnetd_init() at boot.  Spawns one listener kthread that:
 *   1. opens a /dev/tcp stream (stream_build_kernel + push tcp)
 *   2. T_BIND_REQ port=23, T_LISTEN_REQ
 *   3. on T_CONN_IND, builds a fresh responder stream + T_CONN_RES
 *      with the responder TCB
 *   4. spawns a per-connection worker that runs the command loop
 *
 * Because the kappara kernel TPI takes a TCB pointer (not an fd) at
 * accept time, we mirror tcp_selftest_run's direct accept_into path.
 */

#include <stdint.h>

#include "kappara/core/kmem.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"
#include "kappara/io/streams.h"
#include "kappara/io/stream_head.h"
#include "kappara/net/ip.h"
#include "kappara/net/netif.h"
#include "kappara/net/tcp.h"
#include "kappara/proc/sched.h"

#define TELNET_PORT	23
#define TELNET_BANNER \
	"\r\nWelcome to kappara (telnet on QEMU virt + virtio-net).\r\n" \
	"Type 'help' for commands.\r\n"

/* TPI helpers ------------------------------------------------------ */

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

/* Pull the next mblk off sd_rq, blocking on sd_readwait when the
 * queue is empty.  Same shape stream_read uses (sq_lock around the
 * getq + sleep_on window so wakers can't slip in).  Returns NULL
 * only if the stream EOF'd. */
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

static int wait_prim(struct stdata *sd, uint8_t want, void *out, unsigned cap)
{
	for (;;) {
		mblk_t *mp = wait_next_mblk(sd);
		if (!mp) return -1;
		uint8_t prim = (mp->b_wptr - mp->b_rptr) >= 1
		             ? mp->b_rptr[0] : 0;
		if (prim != want) {
			/* Discon arrived before the expected primitive --
			 * connection died, bail. */
			if (prim == T_TCP_DISCON_IND) {
				freemsg(mp);
				return -1;
			}
			freemsg(mp);
			continue;
		}
		if (out && cap) {
			unsigned n = (unsigned)(mp->b_wptr - mp->b_rptr);
			if (n > cap) n = cap;
			kmemcpy(out, mp->b_rptr, n);
		}
		freemsg(mp);
		return 0;
	}
}

/* Block waiting for a T_TCP_DATA_IND, copy its M_DATA payload into
 * the caller's buffer.  Returns bytes received (>0), -1 on close. */
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
		if (prim == T_TCP_ORDREL_IND || prim == T_TCP_DISCON_IND) {
			freemsg(mp);
			return -1;
		}
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

/* ---- Command interpreter --------------------------------------- */

static int eq(const char *a, const char *b)
{
	while (*a && *b && *a == *b) { a++; b++; }
	return *a == 0 && *b == 0;
}

static void cmd_help(struct stdata *sd)
{
	static const char *msg =
		"help   show this list\r\n"
		"ps     thread list\r\n"
		"netstat   IP + TCP state\r\n"
		"ifconfig  registered netifs\r\n"
		"quit   close the session\r\n";
	send_data(sd, msg, kstrlen(msg));
}

/* Simple netif + tcp dump callbacks for use over telnet. */
struct send_ctx { struct stdata *sd; };

static int netif_dump_cb(struct netif *n, void *arg)
{
	struct send_ctx *c = arg;
	char buf[96];
	uint32_t ip = n->ip, nm = n->netmask;
	int o = 0;
	#define EM(p) do { const char *s_=(p); while (*s_ && o<92) buf[o++]=*s_++; } while (0)
	EM(n->name); EM(" ");
	char ipbuf[32];
	int p = 0;
	for (int i = 3; i >= 0; i--) {
		unsigned x = (ip >> (i*8)) & 0xff;
		if (x >= 100) ipbuf[p++] = '0' + x/100;
		if (x >= 10)  ipbuf[p++] = '0' + (x/10)%10;
		ipbuf[p++] = '0' + x%10;
		if (i) ipbuf[p++] = '.';
	}
	ipbuf[p] = 0;
	EM(ipbuf);
	EM("/");
	int mb = 0;
	for (int b = 31; b >= 0; b--) if (nm & (1u<<b)) mb++;
	if (mb >= 10) buf[o++] = '0' + mb/10;
	buf[o++] = '0' + mb%10;
	EM("\r\n");
	#undef EM
	send_data(c->sd, buf, (unsigned)o);
	return 0;
}

static void cmd_ifconfig(struct stdata *sd)
{
	struct send_ctx ctx = { .sd = sd };
	netif_for_each(netif_dump_cb, &ctx);
}

static void tcp_view_cb(const struct tcp_tcb_view *v, void *arg)
{
	struct send_ctx *c = arg;
	char buf[96]; int o = 0;
	#define EM(p) do { const char *s_=(p); while (*s_ && o<92) buf[o++]=*s_++; } while (0)
	EM(v->state_name); EM(" lp=");
	unsigned lp = v->local_port;
	if (lp >= 10000) buf[o++] = '0' + lp/10000;
	if (lp >= 1000)  buf[o++] = '0' + (lp/1000)%10;
	if (lp >= 100)   buf[o++] = '0' + (lp/100)%10;
	if (lp >= 10)    buf[o++] = '0' + (lp/10)%10;
	buf[o++] = '0' + lp%10;
	EM("\r\n");
	#undef EM
	send_data(c->sd, buf, (unsigned)o);
}

static void cmd_netstat(struct stdata *sd)
{
	cmd_ifconfig(sd);
	send_data(sd, "tcp:\r\n", 6);
	struct send_ctx ctx = { .sd = sd };
	tcp_for_each_tcb(tcp_view_cb, &ctx);
}

static void cmd_ps(struct stdata *sd)
{
	send_data(sd, "(ps stub -- /proc/ps walker integration TODO)\r\n", 47);
}

static int run_command(struct stdata *sd, const char *line)
{
	while (*line == ' ') line++;
	if (eq(line, "help"))     { cmd_help(sd);     return 0; }
	if (eq(line, "ps"))       { cmd_ps(sd);       return 0; }
	if (eq(line, "netstat"))  { cmd_netstat(sd);  return 0; }
	if (eq(line, "ifconfig")) { cmd_ifconfig(sd); return 0; }
	if (eq(line, "quit"))     { return 1; }
	if (line[0] == 0)         { return 0; }
	send_data(sd, "?\r\n", 3);
	return 0;
}

/* ---- Per-connection worker ------------------------------------- */

static void telnet_session(struct stdata *R)
{
	send_data(R, TELNET_BANNER, kstrlen(TELNET_BANNER));
	send_data(R, "kappara> ", 9);

	char line[64];
	unsigned lpos = 0;

	for (;;) {
		char buf[64];
		int n = wait_data(R, buf, sizeof(buf));
		if (n < 0) return;
		for (int i = 0; i < n; i++) {
			char c = buf[i];
			if (c == '\r' || c == '\n') {
				line[lpos] = 0;
				send_data(R, "\r\n", 2);
				int q = run_command(R, line);
				lpos = 0;
				if (q) return;
				send_data(R, "kappara> ", 9);
			} else if (c >= ' ' && c < 127 && lpos < sizeof(line) - 1) {
				line[lpos++] = c;
				send_data(R, &c, 1);
			} else if (c == 0x7f || c == 0x08) {
				if (lpos > 0) {
					lpos--;
					send_data(R, "\x08 \x08", 3);
				}
			}
		}
	}
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
	 || wait_prim(L, T_TCP_BIND_ACK, 0, 0) < 0) {
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
	kprintf("telnetd: listening on TCP port %d\n", TELNET_PORT);

	for (;;) {
		struct t_tcp_conn_ind ci;
		if (wait_prim(L, T_TCP_CONN_IND, &ci, sizeof(ci)) < 0) {
			kthread_yield();
			continue;
		}
		kprintf("telnetd: incoming connection from %u.%u.%u.%u:%u\n",
		        (ci.src_ip >> 24) & 0xff, (ci.src_ip >> 16) & 0xff,
		        (ci.src_ip >>  8) & 0xff,  ci.src_ip        & 0xff,
		        ci.src_port);

		/* Build a fresh responder stream + accept onto it via
		 * tcp_accept_into-style direct call.  We don't have a
		 * user fd to pass, so we use the syscall path's q_ptr
		 * dance to convey the responder TCB. */
		struct stdata *R =
		    stream_build_kernel(&ip_streamtab, "telnet_resp", 0);
		if (!R || stream_push_kernel(R, "tcp") < 0) {
			kprintf("telnetd: failed to build responder\n");
			if (R) stream_destroy_kernel(R);
			continue;
		}

		/* Kernel-mode accept: tcp_kaccept reaches the same
		 * tcp_accept_into helper handle_conn_res uses on the
		 * user-fd path, but without going through a userspace
		 * file descriptor. */
		if (tcp_kaccept(L, R) < 0) {
			kprintf("telnetd: tcp_kaccept failed\n");
			stream_destroy_kernel(R);
			continue;
		}

		/* Drain T_CONN_CON on R, then run the per-connection
		 * session.  Graceful close ends the session. */
		if (wait_prim(R, T_TCP_CONN_CON, 0, 0) < 0) {
			stream_destroy_kernel(R);
			continue;
		}
		telnet_session(R);

		struct t_tcp_ordrel_req oq = { .prim = T_TCP_ORDREL_REQ };
		put_prim(R, &oq, sizeof(oq));
		(void)wait_prim(R, T_TCP_ORDREL_IND, 0, 0);
		stream_destroy_kernel(R);
		/* L stays alive; it's still LISTENing on port 23. */
	}
}

void telnetd_init(void)
{
	kthread_create("telnetd", telnetd_main, 0);
}

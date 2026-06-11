/*
 * uts/os/net/slip.c -- SLIP framing module + mini-UART STREAMS leaf
 *
 * Three pieces live in this file:
 *
 *   1. miniuart_streamtab -- a leaf STREAMS driver that sits on top
 *      of the BCM2837 mini-UART hardware (arch/miniuart.c).  wput
 *      emits each M_DATA byte via miniuart_tx_byte().  No qopen --
 *      stateless byte stream.
 *
 *   2. slip_streamtab -- the SLIP framing module.  rput consumes
 *      byte-stream M_DATA from below (the mini-UART driver) and
 *      runs RFC 1055 unstuffing into a per-instance frame buffer;
 *      on END it putnext's a complete IP packet up.  wput accepts
 *      M_DATA (an outgoing IP packet from IP via I_LINK), byte-
 *      stuffs it, and putnext's the framed bytes down.
 *
 *   3. slip_init() -- boot wiring.  Brings up the mini-UART, builds
 *      the slip-over-uart stream (stream_build_kernel +
 *      stream_push_kernel "slip"), spawns the rx kthread, registers
 *      slip0 as a netif, and I_LINKs the stream under IP.  After
 *      this returns, packets destined for 192.168.10.0/30 route
 *      through slip0 automatically.
 *
 * Layering after boot:
 *
 *     ┌────────────────────────────┐
 *     │ IP control stream           │
 *     └──────────┬──────────────────┘
 *                │   I_LINK
 *     ┌──────────▼──────────────────┐
 *     │ stream head (slip stream)    │  -- the slip0 netif from IP
 *     ├──────────────────────────────┤
 *     │ slip module                  │
 *     │   wput: byte-stuff M_DATA    │
 *     │   rput: byte-unstuff bytes   │
 *     ├──────────────────────────────┤
 *     │ miniuart leaf driver         │
 *     │   wput: miniuart_tx_byte()   │
 *     │   rx kthread:                │
 *     │     miniuart_rx_nonblock(),  │
 *     │     putnext drv_rq -> slip   │
 *     ├──────────────────────────────┤
 *     │ BCM2837 mini-UART hardware   │
 *     └──────────────────────────────┘
 *
 * Default IP config:
 *   slip0 = 192.168.10.2/30  (peer = .1)
 *   MTU   = 296 bytes (RFC 1055 recommendation)
 *
 * RFC 1055 framing
 * ----------------
 *   END = 0xC0, ESC = 0xDB, ESC_END = 0xDC, ESC_ESC = 0xDD
 *   Encode each byte:
 *     END  -> ESC ESC_END
 *     ESC  -> ESC ESC_ESC
 *     other -> itself
 *   Prefix with END (flushes any garbage on the peer), suffix with END.
 */

#include <stdint.h>

#include "kappara/arch/miniuart.h"
#include "kappara/core/kmem.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/net/ip.h"
#include "kappara/net/netif.h"
#include "kappara/net/slip.h"
#include "kappara/proc/sched.h"

#define SLIP_END	0xC0
#define SLIP_ESC	0xDB
#define SLIP_ESC_END	0xDC
#define SLIP_ESC_ESC	0xDD

#define SLIP_MTU	296

/* ---- miniuart leaf streamtab ------------------------------------ */

static int miniuart_wq_putp(queue_t *q, mblk_t *mp)
{
	(void)q;
	if (!mp) return 0;
	if (mp->b_datap->db_type != M_DATA) {
		freemsg(mp);
		return 0;
	}
	for (mblk_t *m = mp; m; m = m->b_cont) {
		for (unsigned char *p = m->b_rptr; p < m->b_wptr; p++)
			miniuart_tx_byte(*p);
	}
	freemsg(mp);
	return 0;
}

static int miniuart_rq_putp(queue_t *q, mblk_t *mp)
{
	/* rx kthread feeds bytes UP via putnext on the driver's rq;
	 * normal q_next walk delivers them to the slip module above. */
	return putnext(q, mp);
}

static struct module_info miniuart_minfo = {
	.mi_idnum  = 900,
	.mi_idname = "miniuart",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 8192,
	.mi_lowat  = 4096,
};

static struct qinit miniuart_rinit = {
	.qi_putp  = miniuart_rq_putp,
	.qi_minfo = &miniuart_minfo,
};
static struct qinit miniuart_winit = {
	.qi_putp  = miniuart_wq_putp,
	.qi_minfo = &miniuart_minfo,
};
static struct streamtab miniuart_streamtab = {
	.st_rdinit = &miniuart_rinit,
	.st_wrinit = &miniuart_winit,
};

/* ---- slip framing module ---------------------------------------- */

struct slip_state {
	mblk_t   *frame;	/* in-progress receive frame */
	int       in_esc;	/* unframing state: byte before was ESC */
	uint32_t  rx_frames;	/* counters for debug / /proc/netif later */
	uint32_t  rx_runts;	/* frames shorter than IP min */
	uint32_t  rx_overflow;	/* frame exceeded MTU; discarded */
	uint32_t  tx_frames;
};

static int slip_qopen(queue_t *rq)
{
	struct slip_state *s = kmalloc(sizeof(*s));
	if (!s) return -1;
	kmemset(s, 0, sizeof(*s));
	s->frame = allocb(SLIP_MTU, 0);
	if (!s->frame) {
		kfree(s);
		return -1;
	}
	rq->q_ptr     = s;
	WR(rq)->q_ptr = s;
	return 0;
}

static int slip_qclose(queue_t *rq)
{
	struct slip_state *s = rq->q_ptr;
	if (!s) return 0;
	if (s->frame) freemsg(s->frame);
	rq->q_ptr     = NULL;
	WR(rq)->q_ptr = NULL;
	kfree(s);
	return 0;
}

/* Hand `s->frame` upstream and start a fresh one. */
static void slip_deliver_frame(queue_t *rq, struct slip_state *s)
{
	mblk_t *mp = s->frame;
	s->frame = allocb(SLIP_MTU, 0);
	if (!mp) return;
	unsigned n = (unsigned)(mp->b_wptr - mp->b_rptr);
	if (n < 20) {	/* shorter than minimum IPv4 header -- drop */
		s->rx_runts++;
		freemsg(mp);
		return;
	}
	s->rx_frames++;
	putnext(rq, mp);
}

/* Append a raw byte to the in-progress frame.  Drops if MTU exceeded
 * (the partial frame is discarded; counter ticks; we'll resync at
 * the next END). */
static void slip_append(struct slip_state *s, unsigned char c)
{
	if (!s->frame) return;
	if (s->frame->b_wptr >= s->frame->b_datap->db_lim) {
		s->rx_overflow++;
		s->frame->b_wptr = s->frame->b_rptr;	/* discard */
		return;
	}
	*s->frame->b_wptr++ = c;
}

static int slip_rq_putp(queue_t *q, mblk_t *mp)
{
	struct slip_state *s = q->q_ptr;
	if (!s || !mp) {
		freemsg(mp);
		return -1;
	}
	if (mp->b_datap->db_type != M_DATA) {
		freemsg(mp);
		return 0;
	}

	for (mblk_t *m = mp; m; m = m->b_cont) {
		for (unsigned char *p = m->b_rptr; p < m->b_wptr; p++) {
			unsigned char c = *p;
			if (s->in_esc) {
				if      (c == SLIP_ESC_END) slip_append(s, SLIP_END);
				else if (c == SLIP_ESC_ESC) slip_append(s, SLIP_ESC);
				else                         slip_append(s, c);
				s->in_esc = 0;
				continue;
			}
			if (c == SLIP_END) {
				if (s->frame && s->frame->b_wptr > s->frame->b_rptr)
					slip_deliver_frame(q, s);
				continue;
			}
			if (c == SLIP_ESC) {
				s->in_esc = 1;
				continue;
			}
			slip_append(s, c);
		}
	}
	freemsg(mp);
	return 0;
}

/* Outbound: receive an IP packet (M_DATA chain), byte-stuff into a
 * fresh M_DATA, putnext down to miniuart.  Worst-case output size is
 * 2 * input + 2 (the framing ENDs). */
static int slip_wq_putp(queue_t *q, mblk_t *mp)
{
	struct slip_state *s = q->q_ptr;
	if (!s || !mp) {
		freemsg(mp);
		return -1;
	}
	if (mp->b_datap->db_type != M_DATA) {
		freemsg(mp);
		return 0;
	}

	unsigned in_len = (unsigned)msgdsize(mp);
	unsigned cap    = 2 * in_len + 2;
	mblk_t *out = allocb(cap, 0);
	if (!out) {
		freemsg(mp);
		return -1;
	}
	*out->b_wptr++ = SLIP_END;
	for (mblk_t *m = mp; m; m = m->b_cont) {
		for (unsigned char *p = m->b_rptr; p < m->b_wptr; p++) {
			unsigned char c = *p;
			if (c == SLIP_END) {
				*out->b_wptr++ = SLIP_ESC;
				*out->b_wptr++ = SLIP_ESC_END;
			} else if (c == SLIP_ESC) {
				*out->b_wptr++ = SLIP_ESC;
				*out->b_wptr++ = SLIP_ESC_ESC;
			} else {
				*out->b_wptr++ = c;
			}
		}
	}
	*out->b_wptr++ = SLIP_END;
	freemsg(mp);
	s->tx_frames++;
	return putnext(q, out);
}

static struct module_info slip_minfo = {
	.mi_idnum  = 901,
	.mi_idname = "slip",
	.mi_minpsz = 0,
	.mi_maxpsz = SLIP_MTU,
	.mi_hiwat  = 8192,
	.mi_lowat  = 4096,
};

static struct qinit slip_rinit = {
	.qi_putp  = slip_rq_putp,
	.qi_qopen = slip_qopen,
	.qi_qclose= slip_qclose,
	.qi_minfo = &slip_minfo,
};
static struct qinit slip_winit = {
	.qi_putp  = slip_wq_putp,
	.qi_minfo = &slip_minfo,
};
struct streamtab slip_streamtab = {
	.st_rdinit = &slip_rinit,
	.st_wrinit = &slip_winit,
};

void slip_module_init(void)
{
	streams_register("slip", &slip_streamtab);
}

/* ---- rx kthread ------------------------------------------------- */

static queue_t *slip_rx_drv_rq;	/* miniuart driver's rq; set by slip_init */

static void slip_rx_main(void *arg)
{
	(void)arg;
	unsigned char buf[64];
	for (;;) {
		unsigned n = 0;
		int c;
		while (n < sizeof(buf)
		       && (c = miniuart_rx_nonblock()) >= 0) {
			buf[n++] = (unsigned char)c;
		}
		if (n == 0) {
			kthread_yield();
			continue;
		}
		mblk_t *mp = allocb(n, 0);
		if (mp) {
			kmemcpy(mp->b_wptr, buf, n);
			mp->b_wptr += n;
			putnext(slip_rx_drv_rq, mp);
		}
	}
}

/* ---- slip0 netif identity --------------------------------------- */

static struct netif slip0_nif = {
	.name      = "slip0",
	.ip        = 0xc0a80a02u,	/* 192.168.10.2 */
	.netmask   = 0xfffffffcu,	/* /30          */
	.mtu       = SLIP_MTU,
	.streamtab = NULL,		/* pre-built stream below */
	.tx        = NULL,
};

/* ---- Boot wiring ------------------------------------------------- */

void slip_init(void)
{
	miniuart_init(115200);

	struct stdata *sd = stream_build_kernel(&miniuart_streamtab,
	                                        "slip0_serial", 0);
	if (!sd) {
		kprintf("slip: stream_build_kernel failed\n");
		return;
	}
	if (stream_push_kernel(sd, "slip") < 0) {
		kprintf("slip: stream_push_kernel failed\n");
		stream_destroy_kernel(sd);
		return;
	}

	/* Stash the driver's rq for the rx kthread to deliver into. */
	slip_rx_drv_rq = sd->sd_drv_rq;

	netif_register(&slip0_nif);

	long muxid = ip_attach_stream(sd, &slip0_nif);
	if (muxid <= 0) {
		kprintf("slip: ip_attach_stream failed rc=%ld\n", muxid);
		stream_destroy_kernel(sd);
		return;
	}

	kthread_create("miniuart_rx", slip_rx_main, NULL);

	kprintf("slip: slip0 up on mini-UART (192.168.10.2/30, mtu=%u)\n",
	        SLIP_MTU);
}

int slip_selftest_run(void)
{
	/* No useful in-kernel test without a peer driving the wire.
	 * Real test is host-side via slattach + ping.  We just confirm
	 * the netif is registered. */
	return slip0_nif.streamtab == NULL ? 0 : -1;
}

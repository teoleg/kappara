/*
 * uts/os/net/icmp.c -- ICMP echo handling + /dev/icmp STREAMS cdev
 *
 * The IP layer dispatches ICMP datagrams here.  We auto-reply to
 * type 8 (echo request) by flipping the type byte and recomputing
 * the checksum, then ip_output the same mblk back.  Type 0 (echo
 * reply) is delivered to whichever waiter registered for that id+seq:
 *
 *   Boot waiter (icmp_wq): used by net_selftest.  Arm with
 *     icmp_arm_waiter, poll with icmp_wait_reply.
 *
 *   STREAMS waiters (icmp_sw[]): used by /dev/icmp streams.  Each
 *     open of /dev/icmp gets a slot; the reply mblk is put on the
 *     stream's read queue so user-space read() sees it.
 *
 * Headroom discipline (same as before)
 * -------------------------------------
 * allocb gives us b_rptr==b_wptr==db_base.  ip_output needs
 * IP_HDR_LEN bytes of headroom before b_rptr.  For a fresh echo-
 * request mblk we reserve IP_HDR_LEN at allocation.  For the echo-
 * reply we reuse the incoming mblk which still has its IP headroom
 * intact.
 */

#include <stdint.h>

#include "kappara/cdevsw.h"
#include "kappara/icmp.h"
#include "kappara/ip.h"
#include "kappara/kmem.h"
#include "kappara/netif.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"
#include "kappara/string.h"

/* ---- boot waiter -------------------------------------------------- */

static struct {
	int       waiting;
	uint16_t  id;
	uint16_t  seq;
	int       got;
} icmp_wq;

void icmp_arm_waiter(uint16_t id, uint16_t seq)
{
	icmp_wq.id      = id;
	icmp_wq.seq     = seq;
	icmp_wq.got     = 0;
	icmp_wq.waiting = 1;
}

int icmp_wait_reply(uint16_t id, uint16_t seq, unsigned timeout_ms)
{
	(void)id; (void)seq; (void)timeout_ms;
	for (int i = 0; i < 32 && !icmp_wq.got; i++)
		kthread_yield();

	int r = icmp_wq.got ? 0 : -1;
	icmp_wq.waiting = 0;
	return r;
}

/* ---- STREAMS waiters ---------------------------------------------- */

#define ICMP_SW_SLOTS	4

static struct icmp_sw_slot {
	int      active;
	uint16_t id;
	uint16_t seq;
	uint32_t dst_ip;	/* echoed into rep.src_ip on reply */
	queue_t *rq;		/* driver read-queue for this stream */
} icmp_sw[ICMP_SW_SLOTS];

void icmp_arm_waiter_stream(uint16_t id, uint16_t seq,
                            queue_t *rq, uint32_t dst_ip)
{
	for (int i = 0; i < ICMP_SW_SLOTS; i++) {
		if (icmp_sw[i].active) continue;
		icmp_sw[i].id     = id;
		icmp_sw[i].seq    = seq;
		icmp_sw[i].dst_ip = dst_ip;
		icmp_sw[i].rq     = rq;
		icmp_sw[i].active = 1;
		return;
	}
	kprintf("icmp: no free stream waiter slots\n");
}

/* ---- reply delivery ----------------------------------------------- */

static void icmp_deliver_reply(uint32_t src_ip, uint16_t id, uint16_t seq)
{
	/* Boot waiter. */
	if (icmp_wq.waiting && icmp_wq.id == id && icmp_wq.seq == seq)
		icmp_wq.got = 1;

	/* STREAMS waiters. */
	for (int i = 0; i < ICMP_SW_SLOTS; i++) {
		if (!icmp_sw[i].active) continue;
		if (icmp_sw[i].id != id || icmp_sw[i].seq != seq) continue;

		mblk_t *rmp = allocb(sizeof(struct icmp_ping_rep), 0);
		if (rmp) {
			struct icmp_ping_rep *rep =
			    (struct icmp_ping_rep *)rmp->b_wptr;
			rep->src_ip  = src_ip;
			rep->id      = id;
			rep->seq     = seq;
			rep->rtt_ms  = 0;	/* synchronous loopback */
			rmp->b_wptr += sizeof *rep;
			putnext(icmp_sw[i].rq, rmp);
		}
		icmp_sw[i].active = 0;
		return;
	}
}

/* ---- input -------------------------------------------------------- */

void icmp_input(struct netif *nif, uint32_t src, uint32_t dst, mblk_t *mp)
{
	(void)dst;
	unsigned len = (unsigned)(mp->b_wptr - mp->b_rptr);
	if (len < ICMP_HDR_LEN) {
		kprintf("icmp_input: runt %u\n", len);
		freemsg(mp);
		return;
	}
	struct icmp_hdr *h = (struct icmp_hdr *)mp->b_rptr;

	if (ip_checksum(h, len) != 0) {
		kprintf("icmp_input: checksum fail\n");
		freemsg(mp);
		return;
	}

	if (h->type == ICMP_TYPE_ECHO_REPLY) {
		icmp_deliver_reply(src, ntohs16(h->id), ntohs16(h->seq));
		freemsg(mp);
		return;
	}

	if (h->type == ICMP_TYPE_ECHO_REQUEST) {
		h->type     = ICMP_TYPE_ECHO_REPLY;
		h->checksum = 0;
		uint16_t cs = ip_checksum(h, len);
		h->checksum = htons16(cs);
		(void)nif;
		if (ip_output(src, IPPROTO_ICMP, mp) < 0) {
			/* ip_output freed mp on failure */
		}
		return;
	}

	freemsg(mp);
}

/* ---- outgoing echo ------------------------------------------------ */

int icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                   const void *payload, unsigned len)
{
	unsigned sz = IP_HDR_LEN + ICMP_HDR_LEN + len;
	mblk_t *mp = allocb(sz, 0);
	if (!mp) return -1;

	mp->b_rptr += IP_HDR_LEN;
	mp->b_wptr  = mp->b_rptr;

	struct icmp_hdr *h = (struct icmp_hdr *)mp->b_wptr;
	h->type     = ICMP_TYPE_ECHO_REQUEST;
	h->code     = 0;
	h->checksum = 0;
	h->id       = htons16(id);
	h->seq      = htons16(seq);
	mp->b_wptr += ICMP_HDR_LEN;

	if (len > 0 && payload) {
		kmemcpy(mp->b_wptr, payload, len);
		mp->b_wptr += len;
	}

	uint16_t cs = ip_checksum(h, ICMP_HDR_LEN + len);
	h->checksum = htons16(cs);

	return ip_output(dst_ip, IPPROTO_ICMP, mp);
}

/* ---- /dev/icmp STREAMS driver ------------------------------------- */
/*
 * User-facing interface:
 *   write: one M_DATA block containing struct icmp_ping_req.
 *   read:  one M_DATA block containing struct icmp_ping_rep.
 *
 * lo0 is synchronous so the reply arrives inside the icmp_send_echo
 * call in icmp_drv_wq_putp; the reply mblk is already on the stream-
 * head read queue before wput returns.  The user can read() right
 * after write() with no blocking needed.
 */

static int icmp_drv_wq_putp(queue_t *q, mblk_t *mp)
{
	if (!mp) return 0;
	if (mp->b_datap->db_type != M_DATA) {
		freemsg(mp);
		return 0;
	}
	unsigned len = (unsigned)(mp->b_wptr - mp->b_rptr);
	if (len < sizeof(struct icmp_ping_req)) {
		kprintf("icmp_drv: short write %u\n", len);
		freemsg(mp);
		return 0;
	}

	const struct icmp_ping_req *req =
	    (const struct icmp_ping_req *)mp->b_rptr;
	uint32_t dst_ip = req->dst_ip;
	uint16_t id     = req->id;
	uint16_t seq    = req->seq;
	freemsg(mp);

	/* Arm the STREAMS waiter before sending so the synchronous reply
	 * from lo0 finds the slot ready. */
	queue_t *drv_rq = OTHERQ(q);
	icmp_arm_waiter_stream(id, seq, drv_rq, dst_ip);
	icmp_send_echo(dst_ip, id, seq, NULL, 0);
	return 0;
}

static int icmp_drv_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static struct module_info icmp_drv_minfo = {
	.mi_idnum  = 401,
	.mi_idname = "icmp",
	.mi_minpsz = 0,
	.mi_maxpsz = 1500,
	.mi_hiwat  = 8192,
	.mi_lowat  = 4096,
};

static struct qinit icmp_drv_rinit = {
	.qi_putp  = icmp_drv_rq_putp,
	.qi_minfo = &icmp_drv_minfo,
};
static struct qinit icmp_drv_winit = {
	.qi_putp  = icmp_drv_wq_putp,
	.qi_minfo = &icmp_drv_minfo,
};
static struct streamtab icmp_streamtab = {
	.st_rdinit = &icmp_drv_rinit,
	.st_wrinit = &icmp_drv_winit,
};

void icmp_str_init(void)
{
	streams_register("icmp", &icmp_streamtab);
	cdev_register(CDEV_MAJ_ICMP, "icmp", &icmp_streamtab);
}

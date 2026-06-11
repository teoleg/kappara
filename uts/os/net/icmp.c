/*
 * uts/os/net/icmp.c -- ICMP as a STREAMS module above IP
 *
 * After N2c ICMP is a real SVR4-style STREAMS module: not a driver,
 * not a direct callee of IP.  It sits between the stream head and
 * the IP driver, gets pushed into the stack by /dev/icmp's autopush
 * rule (or any I_PUSH "icmp"), and exchanges M_PROTO + M_DATA with
 * IP via the standard putnext / put STREAMS primitives.
 *
 * Per-instance state (struct icmp_state):
 *   icmp_id:    16-bit id auto-assigned at qopen.  Stamped into
 *               every outgoing echo request and filter-matched on
 *               every incoming echo reply.
 *   bound:      0/1 -- have we received IP_T_BIND_ACK yet?  Until
 *               so, writes are dropped.
 *   ping_seq:   most recently armed echo seq (single-slot waiter).
 *               Future work: turn into a per-(id,seq) list if we
 *               ever want pipelined pings.
 *   ping_got:   reply with matching (icmp_id, ping_seq) has landed.
 *
 * Receive-side filtering: every demuxed proto=1 packet that lands
 * in icmp_rq_putp is filtered by `icmp_id` BEFORE anything else.
 * Mismatches are silently dropped.  This is what lets two concurrent
 * /dev/icmp opens coexist -- each bound to proto=1, each filtering
 * by its own id, both seeing only its own traffic.
 *
 * Auto-reply: when a packet matches our id AND is type=8 (echo
 * request), we flip the type byte, recompute the checksum, and
 * putnext(WR(q), M_PROTO{IP_T_SEND_REQ} + M_DATA(reply)) -- the
 * reply goes back DOWN through the same stack to IP.  For the
 * loopback selftest this is the case (we send to ourselves, the
 * request comes back via lo0 with our id, we auto-reply, the
 * reply also comes back with our id, the waiter fires).
 */

#include <stdint.h>

#include "kappara/cdevsw.h"
#include "kappara/kmem.h"
#include "kappara/net/icmp.h"
#include "kappara/net/ip.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"
#include "kappara/string.h"

/* ---- Per-instance state ------------------------------------------- */

struct icmp_state {
	uint16_t   icmp_id;
	int        bound;
	/* Single-slot reply waiter.  Set by wput before sending; cleared
	 * by rq_putp when a matching reply arrives.  read() polls until
	 * the response mblk shows up on the stream head's read queue. */
	uint16_t   ping_seq;
	int        ping_armed;
	uint32_t   ping_src_ip;	/* filled by rq_putp on reply */
};

/* Next id to hand out.  Skips zero -- some old listeners treat
 * id=0 as "unassigned" and drop the packet. */
static uint16_t icmp_id_next = 1;

static uint16_t alloc_icmp_id(void)
{
	uint16_t id = icmp_id_next++;
	if (icmp_id_next == 0) icmp_id_next = 1;
	return id;
}

/* ---- Helpers ------------------------------------------------------ */

static void icmp_send_bind(queue_t *rq, uint8_t prim)
{
	mblk_t *mp = allocb(sizeof(struct ip_bind_meta), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct ip_bind_meta *m = (struct ip_bind_meta *)mp->b_wptr;
	m->prim   = prim;
	m->proto  = IPPROTO_ICMP;
	m->_pad[0] = m->_pad[1] = 0;
	m->key    = 0;
	mp->b_wptr += sizeof(*m);
	putnext(WR(rq), mp);
}

/* Build an ICMP body mblk (header + payload) with IP_HDR_LEN
 * headroom reserved so IP can prepend in place.  Caller fills
 * type and computes checksum; we just lay out the bytes. */
static mblk_t *icmp_alloc_body(uint8_t type, uint16_t id, uint16_t seq,
                               const void *payload, unsigned plen)
{
	unsigned sz = IP_HDR_LEN + ICMP_HDR_LEN + plen;
	mblk_t *mp = allocb(sz, 0);
	if (!mp) return NULL;
	mp->b_rptr += IP_HDR_LEN;
	mp->b_wptr  = mp->b_rptr;

	struct icmp_hdr *h = (struct icmp_hdr *)mp->b_wptr;
	h->type     = type;
	h->code     = 0;
	h->checksum = 0;
	h->id       = htons16(id);
	h->seq      = htons16(seq);
	mp->b_wptr += ICMP_HDR_LEN;

	if (plen > 0 && payload) {
		kmemcpy(mp->b_wptr, payload, plen);
		mp->b_wptr += plen;
	}

	uint16_t cs = ip_checksum(h, ICMP_HDR_LEN + plen);
	h->checksum = htons16(cs);
	return mp;
}

/* Wrap an ICMP body mblk in an M_PROTO{IP_T_SEND_REQ} envelope
 * and send it down the stream.  Used by both the user-write path
 * (qopen wput -> ip) and the auto-reply path (rq_putp -> ip via
 * WR(q)).  `q_for_putnext` is the queue whose q_next leads down
 * to IP. */
static int icmp_send_to_ip(queue_t *q_for_putnext, uint32_t dst,
                           mblk_t *body)
{
	mblk_t *meta = allocb(sizeof(struct ip_send_meta), 0);
	if (!meta) {
		freemsg(body);
		return -1;
	}
	meta->b_datap->db_type = M_PROTO;
	struct ip_send_meta *m = (struct ip_send_meta *)meta->b_wptr;
	m->prim   = IP_T_SEND_REQ;
	m->proto  = IPPROTO_ICMP;
	m->_pad[0] = m->_pad[1] = 0;
	m->dst_ip = dst;
	meta->b_wptr += sizeof(*m);
	meta->b_cont  = body;
	return putnext(q_for_putnext, meta);
}

/* ---- qopen / qclose ---------------------------------------------- */

static int icmp_qopen(queue_t *rq)
{
	struct icmp_state *s = kmalloc(sizeof(*s));
	if (!s) return -1;
	kmemset(s, 0, sizeof(*s));
	s->icmp_id = alloc_icmp_id();
	rq->q_ptr        = s;
	WR(rq)->q_ptr    = s;
	icmp_send_bind(rq, IP_T_BIND_REQ);
	return 0;
}

static int icmp_qclose(queue_t *rq)
{
	struct icmp_state *s = rq->q_ptr;
	if (!s) return 0;
	if (s->bound)
		icmp_send_bind(rq, IP_T_UNBIND_REQ);
	rq->q_ptr     = NULL;
	WR(rq)->q_ptr = NULL;
	kfree(s);
	return 0;
}

/* ---- Write side -------------------------------------------------- */

static int icmp_wq_putp(queue_t *q, mblk_t *mp)
{
	struct icmp_state *s = q->q_ptr;
	if (!s || !mp) {
		freemsg(mp);
		return -1;
	}
	if (mp->b_datap->db_type != M_DATA) {
		freemsg(mp);
		return -1;
	}
	if ((mp->b_wptr - mp->b_rptr) < (int)sizeof(struct icmp_ping_req)) {
		freemsg(mp);
		return -1;
	}
	struct icmp_ping_req req = *(struct icmp_ping_req *)mp->b_rptr;
	freemsg(mp);

	if (!s->bound) {
		kprintf("icmp: write before bind ack\n");
		return -1;
	}

	/* Arm waiter before sending; lo0 is synchronous so the reply
	 * comes back inside this same call chain. */
	s->ping_seq    = req.seq;
	s->ping_armed  = 1;
	s->ping_src_ip = 0;

	mblk_t *body = icmp_alloc_body(ICMP_TYPE_ECHO_REQUEST,
	                               s->icmp_id, req.seq, NULL, 0);
	if (!body) {
		s->ping_armed = 0;
		return -1;
	}
	return icmp_send_to_ip(q, req.dst_ip, body);
}

/* ---- Read side -- demuxed packets arrive here from IP ------------ */

static void icmp_handle_ack_nak(struct icmp_state *s, mblk_t *mp)
{
	struct ip_bind_meta *m = (struct ip_bind_meta *)mp->b_rptr;
	if (m->prim == IP_T_BIND_ACK)
		s->bound = 1;
	else if (m->prim == IP_T_BIND_NAK)
		s->bound = 0;
	freemsg(mp);
}

static int icmp_rq_putp(queue_t *q, mblk_t *mp)
{
	struct icmp_state *s = q->q_ptr;
	if (!s || !mp) {
		freemsg(mp);
		return -1;
	}

	/* M_PROTO is either an IP bind handshake reply (IP_T_BIND_ACK,
	 * IP_T_BIND_NAK, IP_T_UNBIND_ACK) or IP_T_UNITDATA_IND (inbound
	 * demuxed packet header carrying src_ip + dst_ip with the M_DATA
	 * payload chained as b_cont).  Same first-byte tagged-union
	 * dispatch the rest of the stack uses. */
	if (mp->b_datap->db_type != M_PROTO) {
		freemsg(mp);
		return 0;
	}
	if ((mp->b_wptr - mp->b_rptr) < 1) {
		freemsg(mp);
		return 0;
	}
	uint8_t prim = mp->b_rptr[0];

	if (prim == IP_T_BIND_ACK || prim == IP_T_BIND_NAK
	 || prim == IP_T_UNBIND_ACK) {
		icmp_handle_ack_nak(s, mp);
		return 0;
	}

	if (prim != IP_T_UNITDATA_IND
	    || (mp->b_wptr - mp->b_rptr)
	       < (int)sizeof(struct ip_unitdata_ind)
	    || !mp->b_cont) {
		freemsg(mp);
		return 0;
	}

	struct ip_unitdata_ind *ind = (struct ip_unitdata_ind *)mp->b_rptr;
	uint32_t src_ip = ind->src_ip;
	mblk_t *body = mp->b_cont;
	mp->b_cont = NULL;
	freeb(mp);	/* free just the M_PROTO header; body retained */

	/* Validate ICMP header. */
	unsigned len = (unsigned)(body->b_wptr - body->b_rptr);
	if (len < ICMP_HDR_LEN) {
		freemsg(body);
		return 0;
	}
	struct icmp_hdr *h = (struct icmp_hdr *)body->b_rptr;
	if (ip_checksum(h, len) != 0) {
		freemsg(body);
		return 0;
	}

	uint16_t pkt_id  = ntohs16(h->id);
	uint16_t pkt_seq = ntohs16(h->seq);

	/* Per-instance id filter: drop anything not addressed to us. */
	if (pkt_id != s->icmp_id) {
		freemsg(body);
		return 0;
	}

	if (h->type == ICMP_TYPE_ECHO_REQUEST) {
		/* Auto-reply.  Flip type, recompute checksum, send back
		 * to the original source carried in the IND. */
		h->type     = ICMP_TYPE_ECHO_REPLY;
		h->checksum = 0;
		uint16_t cs = ip_checksum(h, len);
		h->checksum = htons16(cs);
		(void)icmp_send_to_ip(WR(q), src_ip, body);
		return 0;
	}

	if (h->type == ICMP_TYPE_ECHO_REPLY) {
		if (s->ping_armed && s->ping_seq == pkt_seq) {
			s->ping_armed  = 0;
			s->ping_src_ip = src_ip;

			/* Deliver the formatted reply UP to the stream
			 * head so the user's read() sees it. */
			mblk_t *rmp = allocb(sizeof(struct icmp_ping_rep), 0);
			if (rmp) {
				struct icmp_ping_rep *rep =
				    (struct icmp_ping_rep *)rmp->b_wptr;
				rep->src_ip = s->ping_src_ip;
				rep->seq    = pkt_seq;
				rep->rtt_ms = 0;
				rmp->b_wptr += sizeof(*rep);
				putnext(q, rmp);
			}
		}
		freemsg(body);
		return 0;
	}

	freemsg(body);
	return 0;
}

/* ---- Module wiring ----------------------------------------------- */

static struct module_info icmp_minfo = {
	.mi_idnum  = 401,
	.mi_idname = "icmp",
	.mi_minpsz = 0,
	.mi_maxpsz = 1500,
	.mi_hiwat  = 8192,
	.mi_lowat  = 4096,
};

static struct qinit icmp_rinit = {
	.qi_putp  = icmp_rq_putp,
	.qi_qopen = icmp_qopen,
	.qi_qclose= icmp_qclose,
	.qi_minfo = &icmp_minfo,
};
static struct qinit icmp_winit = {
	.qi_putp  = icmp_wq_putp,
	.qi_minfo = &icmp_minfo,
};
struct streamtab icmp_streamtab = {
	.st_rdinit = &icmp_rinit,
	.st_wrinit = &icmp_winit,
};

void icmp_module_init(void)
{
	streams_register("icmp", &icmp_streamtab);
	/* /dev/icmp's cdevsw entry hands stream_open the IP streamtab;
	 * the autopush block in stream_open then I_PUSHes "icmp" on
	 * top.  Same shape ldterm uses for tty. */
	extern struct streamtab ip_streamtab;
	cdev_register(CDEV_MAJ_ICMP, "icmp", &ip_streamtab);
}

/* ---- In-kernel selftest ------------------------------------------- */
/*
 * The boot selftest builds an icmp-over-ip stream in-kernel, sends
 * a request, drains the reply.  Identical to what cmd/ping does in
 * user space, just without the syscall layer.
 */

int icmp_selftest_run(uint32_t dst, uint16_t seq)
{
	extern struct streamtab ip_streamtab;
	struct stdata *sd = stream_build_kernel(&ip_streamtab,
	                                        "icmp_selftest", 0);
	if (!sd) return -1;

	/* Push icmp on top.  No public helper for in-kernel push, so
	 * fake it: build an ioctl mblk... actually the cleanest is to
	 * use the stream-builder + a kernel push.  We expose
	 * do_ipush by going through stream_ioctl_kernel -- not done;
	 * instead, register an alternative "icmp-direct" streamtab
	 * that IS the icmp module (no IP underneath).  But then the
	 * data path needs IP below.  Solution: open via I_PUSH path
	 * through the public file_ops layer.
	 *
	 * Simpler: stream_build_kernel(icmp_streamtab) makes a stream
	 * where icmp IS the driver.  But then there's no IP below and
	 * its wput would putnext into NULL.  No good.
	 *
	 * Use stream_push_kernel: a new exported helper. */
	extern int stream_push_kernel(struct stdata *sd, const char *modname);
	if (stream_push_kernel(sd, "icmp") < 0) {
		stream_destroy_kernel(sd);
		return -1;
	}

	/* Write the request. */
	mblk_t *mp = allocb(sizeof(struct icmp_ping_req), 0);
	if (!mp) { stream_destroy_kernel(sd); return -1; }
	struct icmp_ping_req *req = (struct icmp_ping_req *)mp->b_wptr;
	req->dst_ip = dst;
	req->seq    = seq;
	req->_pad   = 0;
	mp->b_wptr += sizeof(*req);
	putnext(sd->sd_wq, mp);

	/* Read the reply -- lo0 is synchronous so it should already
	 * be on the head read queue. */
	mblk_t *got = NULL;
	for (int i = 0; i < 32 && !got; i++) {
		got = getq(sd->sd_rq);
		if (!got) kthread_yield();
	}

	int rc = -1;
	if (got) {
		if ((got->b_wptr - got->b_rptr)
		    >= (int)sizeof(struct icmp_ping_rep)) {
			struct icmp_ping_rep *rep =
			    (struct icmp_ping_rep *)got->b_rptr;
			if (rep->seq == seq) rc = 0;
		}
		freemsg(got);
	}

	stream_destroy_kernel(sd);
	return rc;
}

int net_run_icmp_selftest(uint32_t dst, uint16_t seq)
{
	return icmp_selftest_run(dst, seq);
}

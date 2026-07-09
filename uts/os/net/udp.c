/*
 * uts/os/net/udp.c -- UDP as a STREAMS module above IP.
 *
 * Same shape as ICMP: a pushable module, not a driver.  /dev/udp's
 * cdev maps to ip_streamtab; stream_open autopushes "udp" on top.
 * Per-open state lives in struct udp_state behind q_ptr.
 *
 * User ABI is TPI-flavoured: putmsg(M_PROTO{t_*_req} + M_DATA(payload))
 * down, getmsg(M_PROTO{t_*_ind|ack|nak} + M_DATA(payload)) up.
 *
 * Bind / unbind
 * -------------
 *   user putmsg T_BIND_REQ{port=N}
 *     -> udp_wq_putp records local_port = N, sends M_PROTO{IP_T_BIND_REQ,
 *        proto=17, key=N} down to IP.
 *   IP records (proto=17, key=N, upper_rq) in ip_uppers[].  If the
 *   (proto, key) pair is already taken by another endpoint, IP NAKs.
 *   The M_PROTO ACK/NAK flows back up to our rput, which translates
 *   it to T_BIND_ACK / T_BIND_NAK and putnexts UP to the stream head.
 *   Port 0 means "any" -- we pick an ephemeral via local counter.
 *
 * Send (T_UNITDATA_REQ)
 * ---------------------
 *   user putmsg(M_PROTO{T_UNITDATA_REQ, dst_ip, dst_port}, M_DATA(payload))
 *     -> udp_wq_putp validates we're bound, allocates a fresh mblk
 *        with IP_HDR_LEN+UDP_HDR_LEN headroom, copies payload, writes
 *        UDP header (src_port=our local_port, dst_port from request,
 *        len = UDP_HDR_LEN + payload, checksum = 0), wraps in
 *        M_PROTO{IP_T_SEND_REQ, dst_ip, proto=17} + M_DATA, putnexts
 *        down.  The original ctl + data mblks are freed.
 *
 * Receive (T_UNITDATA_IND)
 * ------------------------
 *   IP demux finds our (proto=17, *) entry, put()s the M_DATA payload
 *   (UDP header + body) into our rq.
 *     -> udp_rq_putp parses the UDP header, checks dst_port matches
 *        our local_port (the IP demux didn't check the key, so the
 *        filter is here).  Builds M_PROTO{T_UNITDATA_IND, src_ip,
 *        src_port} + M_DATA(stripped body), putnexts UP.
 *   The user's getmsg returns the (ctl, data) pair.
 *
 * Lifecycle
 * ---------
 *   qopen alloc state, no bind yet.  qclose sends IP_T_UNBIND_REQ if
 *   we were bound, frees state.
 */

#include <stdint.h>

#include "kappara/io/cdevsw.h"
#include "kappara/core/kmem.h"
#include "kappara/net/ip.h"
#include "kappara/net/udp.h"
#include "kappara/core/printk.h"
#include "kappara/proc/sched.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/core/string.h"

/* ---- Per-instance state ------------------------------------------- */

struct udp_state {
	uint16_t   local_port;	/* 0 = unbound */
	int        bound;	/* IP_T_BIND_ACK received */
};

/* Ephemeral port allocator.  Real BSD ranges 49152..65535; we start
 * lower since nothing else binds.  Skip well-known ports (<1024). */
static uint16_t udp_ephemeral_next = 32768;

static uint16_t alloc_ephemeral_port(void)
{
	uint16_t p = udp_ephemeral_next++;
	if (udp_ephemeral_next == 0) udp_ephemeral_next = 32768;
	return p;
}

/* ---- IP M_PROTO helpers ------------------------------------------- */

static void send_ip_bind(queue_t *rq, uint8_t prim, uint16_t port)
{
	mblk_t *mp = allocb(sizeof(struct ip_bind_meta), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct ip_bind_meta *m = (struct ip_bind_meta *)mp->b_wptr;
	m->prim   = prim;
	m->proto  = IPPROTO_UDP;
	m->_pad[0] = m->_pad[1] = 0;
	m->key    = port;
	mp->b_wptr += sizeof(*m);
	putnext(WR(rq), mp);
}

/* ---- TPI reply helpers (kernel -> user, up the stream) ------------ */

static void reply_bind_ack(queue_t *rq, uint16_t port)
{
	mblk_t *mp = allocb(sizeof(struct t_bind_ack), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct t_bind_ack *a = (struct t_bind_ack *)mp->b_wptr;
	a->prim    = T_BIND_ACK;
	a->_pad[0] = 0;
	a->port    = port;
	mp->b_wptr += sizeof(*a);
	putnext(rq, mp);
}

static void reply_bind_nak(queue_t *rq, uint8_t reason)
{
	mblk_t *mp = allocb(sizeof(struct t_bind_nak), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct t_bind_nak *n = (struct t_bind_nak *)mp->b_wptr;
	n->prim    = T_BIND_NAK;
	n->reason  = reason;
	n->_pad[0] = n->_pad[1] = 0;
	mp->b_wptr += sizeof(*n);
	putnext(rq, mp);
}

/* ---- qopen / qclose ---------------------------------------------- */

static int udp_qopen(queue_t *rq)
{
	struct udp_state *s = kmalloc(sizeof(*s));
	if (!s) return -1;
	kmemset(s, 0, sizeof(*s));
	rq->q_ptr     = s;
	WR(rq)->q_ptr = s;
	/* No automatic bind -- user must T_BIND_REQ first. */
	return 0;
}

static int udp_qclose(queue_t *rq)
{
	struct udp_state *s = rq->q_ptr;
	if (!s) return 0;
	if (s->bound)
		send_ip_bind(rq, IP_T_UNBIND_REQ, s->local_port);
	rq->q_ptr     = NULL;
	WR(rq)->q_ptr = NULL;
	kfree(s);
	return 0;
}

/* ---- Write side -- TPI primitives from user ---------------------- */

static int handle_bind_req(queue_t *q, struct udp_state *s,
                           const struct t_bind_req *req)
{
	if (s->bound) {
		reply_bind_nak(OTHERQ(q), 1);
		return 0;
	}
	uint16_t port = req->port ? req->port : alloc_ephemeral_port();
	s->local_port = port;
	/* Send IP bind down; ip rput will reply with IP_T_BIND_ACK or
	 * IP_T_BIND_NAK which our rq_putp translates to T_BIND_ACK /
	 * T_BIND_NAK going up. */
	send_ip_bind(OTHERQ(q), IP_T_BIND_REQ, port);
	return 0;
}

static int handle_unitdata_req(queue_t *q, struct udp_state *s,
                               const struct t_unitdata_req *req,
                               mblk_t *payload)
{
	if (!s->bound) {
		freemsg(payload);
		return -1;
	}
	if (!payload) {
		/* zero-byte UDP -- allocate an empty M_DATA for the
		 * UDP-only frame */
		payload = allocb(IP_HDR_LEN + UDP_HDR_LEN, 0);
		if (!payload) return -1;
		payload->b_rptr += IP_HDR_LEN + UDP_HDR_LEN;
		payload->b_wptr  = payload->b_rptr;
	}

	/* User-supplied payload may not have any headroom for the UDP
	 * header or the IP header.  Copy into a fresh mblk that
	 * reserves IP_HDR_LEN + UDP_HDR_LEN bytes.  This is the cleanest
	 * path; once we add zero-copy sendmsg we can be smarter. */
	unsigned plen = (unsigned)(payload->b_wptr - payload->b_rptr);
	mblk_t *mp = allocb(IP_HDR_LEN + UDP_HDR_LEN + plen, 0);
	if (!mp) {
		freemsg(payload);
		return -1;
	}
	mp->b_rptr += IP_HDR_LEN + UDP_HDR_LEN;
	mp->b_wptr  = mp->b_rptr;
	if (plen > 0) {
		kmemcpy(mp->b_wptr, payload->b_rptr, plen);
		mp->b_wptr += plen;
	}
	freemsg(payload);

	/* Prepend UDP header in the reserved headroom. */
	mp->b_rptr -= UDP_HDR_LEN;
	struct udp_hdr *h = (struct udp_hdr *)mp->b_rptr;
	h->src_port = htons16(s->local_port);
	h->dst_port = htons16(req->dst_port);
	h->len      = htons16((uint16_t)(UDP_HDR_LEN + plen));
	h->checksum = 0;

	/* Wrap in M_PROTO{IP_T_SEND_REQ} + M_DATA, putnext down. */
	mblk_t *meta = allocb(sizeof(struct ip_send_meta), 0);
	if (!meta) {
		freemsg(mp);
		return -1;
	}
	meta->b_datap->db_type = M_PROTO;
	struct ip_send_meta *m = (struct ip_send_meta *)meta->b_wptr;
	m->prim   = IP_T_SEND_REQ;
	m->proto  = IPPROTO_UDP;
	m->_pad[0] = m->_pad[1] = 0;
	m->dst_ip = req->dst_ip;
	meta->b_wptr += sizeof(*m);
	meta->b_cont  = mp;

	return putnext(q, meta);
}

static int udp_wq_putp(queue_t *q, mblk_t *mp)
{
	struct udp_state *s = q->q_ptr;
	if (!s || !mp) {
		freemsg(mp);
		return -1;
	}

	/* M_IOCTL passes through to IP below -- /dev/udp is the
	 * canonical stream for interface plumbing ioctls (SIOCSIF*,
	 * net/sockio.h), exactly how Solaris does it.  IP answers with
	 * M_IOCACK/M_IOCNAK which udp_rq_putp passes back up. */
	if (mp->b_datap->db_type == M_IOCTL)
		return putnext(q, mp);

	/* TPI primitives arrive as M_PROTO with a leading prim byte;
	 * payload (if any) is in b_cont as M_DATA. */
	if (mp->b_datap->db_type != M_PROTO) {
		freemsg(mp);
		return -1;
	}
	if ((mp->b_wptr - mp->b_rptr) < 1) {
		freemsg(mp);
		return -1;
	}
	uint8_t prim = mp->b_rptr[0];

	int rc = -1;
	if (prim == T_BIND_REQ
	    && (mp->b_wptr - mp->b_rptr) >= (int)sizeof(struct t_bind_req)) {
		rc = handle_bind_req(q, s, (struct t_bind_req *)mp->b_rptr);
	} else if (prim == T_UNITDATA_REQ
	    && (mp->b_wptr - mp->b_rptr)
	       >= (int)sizeof(struct t_unitdata_req)) {
		mblk_t *payload = mp->b_cont;
		mp->b_cont = NULL;
		rc = handle_unitdata_req(q, s,
		                         (struct t_unitdata_req *)mp->b_rptr,
		                         payload);
	}
	freemsg(mp);
	return rc;
}

/* ---- Read side -- packets from IP and IP bind ACKs --------------- */

static void handle_ip_ack(queue_t *q, struct udp_state *s, mblk_t *mp)
{
	struct ip_bind_meta *m = (struct ip_bind_meta *)mp->b_rptr;
	uint8_t prim = m->prim;
	uint16_t port = (uint16_t)m->key;
	freemsg(mp);

	if (prim == IP_T_BIND_ACK) {
		s->bound = 1;
		reply_bind_ack(q, port);
	} else if (prim == IP_T_BIND_NAK) {
		s->bound      = 0;
		s->local_port = 0;
		reply_bind_nak(q, 1);
	}
	/* IP_T_UNBIND_ACK: silent. */
}

static void deliver_unitdata_ind(queue_t *q, uint32_t src_ip,
                                 uint16_t src_port, mblk_t *body)
{
	mblk_t *ctl = allocb(sizeof(struct t_unitdata_ind), 0);
	if (!ctl) {
		freemsg(body);
		return;
	}
	ctl->b_datap->db_type = M_PROTO;
	struct t_unitdata_ind *ind = (struct t_unitdata_ind *)ctl->b_wptr;
	ind->prim    = T_UNITDATA_IND;
	ind->_pad[0] = ind->_pad[1] = ind->_pad[2] = 0;
	ind->src_ip  = src_ip;
	ind->src_port = src_port;
	ind->_pad2   = 0;
	ctl->b_wptr += sizeof(*ind);
	ctl->b_cont  = body;
	putnext(q, ctl);
}

static int udp_rq_putp(queue_t *q, mblk_t *mp)
{
	struct udp_state *s = q->q_ptr;
	if (!s || !mp) {
		freemsg(mp);
		return -1;
	}

	/* ioctl responses from IP go straight up to the stream head,
	 * which rendezvouses them with the waiting strioctl caller. */
	if (mp->b_datap->db_type == M_IOCACK
	 || mp->b_datap->db_type == M_IOCNAK)
		return putnext(q, mp);

	/* Everything from IP is M_PROTO -- either bind/unbind ack
	 * (handshake) or IP_T_UNITDATA_IND (inbound packet header +
	 * M_DATA payload chained as b_cont).  First byte selects. */
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
		handle_ip_ack(q, s, mp);
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
	freeb(mp);	/* free just the M_PROTO header */

	unsigned len = (unsigned)(body->b_wptr - body->b_rptr);
	if (len < UDP_HDR_LEN) {
		freemsg(body);
		return 0;
	}
	struct udp_hdr *h = (struct udp_hdr *)body->b_rptr;
	uint16_t sport = ntohs16(h->src_port);
	uint16_t dport = ntohs16(h->dst_port);
	uint16_t ulen  = ntohs16(h->len);
	if (ulen > len || ulen < UDP_HDR_LEN) {
		freemsg(body);
		return 0;
	}
	if (dport != s->local_port) {
		freemsg(body);
		return 0;
	}

	body->b_rptr += UDP_HDR_LEN;
	deliver_unitdata_ind(q, src_ip, sport, body);
	return 0;
}

/* ---- Module wiring ----------------------------------------------- */

static struct module_info udp_minfo = {
	.mi_idnum  = 700,
	.mi_idname = "udp",
	.mi_minpsz = 0,
	.mi_maxpsz = 1500,
	.mi_hiwat  = 8192,
	.mi_lowat  = 4096,
};

static struct qinit udp_rinit = {
	.qi_putp  = udp_rq_putp,
	.qi_qopen = udp_qopen,
	.qi_qclose= udp_qclose,
	.qi_minfo = &udp_minfo,
};
static struct qinit udp_winit = {
	.qi_putp  = udp_wq_putp,
	.qi_minfo = &udp_minfo,
};
struct streamtab udp_streamtab = {
	.st_rdinit = &udp_rinit,
	.st_wrinit = &udp_winit,
};

void udp_module_init(void)
{
	streams_register("udp", &udp_streamtab);
	cdev_register(CDEV_MAJ_UDP, "udp", &ip_streamtab);
}

/* ---- In-kernel selftest ------------------------------------------ */

int udp_selftest_run(void)
{
	struct stdata *sd = stream_build_kernel(&ip_streamtab,
	                                        "udp_selftest", 0);
	if (!sd) return -1;
	if (stream_push_kernel(sd, "udp") < 0) {
		stream_destroy_kernel(sd);
		return -1;
	}

	const uint16_t port = 12345;
	int rc = -1;

	/* T_BIND_REQ */
	{
		mblk_t *mp = allocb(sizeof(struct t_bind_req), 0);
		if (!mp) goto out;
		mp->b_datap->db_type = M_PROTO;
		struct t_bind_req *r = (struct t_bind_req *)mp->b_wptr;
		r->prim    = T_BIND_REQ;
		r->_pad[0] = 0;
		r->port    = port;
		mp->b_wptr += sizeof(*r);
		putnext(sd->sd_wq, mp);
	}

	/* Drain the T_BIND_ACK from the head's rq. */
	mblk_t *got = NULL;
	for (int i = 0; i < 32 && !got; i++) {
		got = getq(sd->sd_rq);
		if (!got) kthread_yield();
	}
	if (!got) goto out;
	if (got->b_datap->db_type != M_PROTO
	    || got->b_rptr[0] != T_BIND_ACK) {
		freemsg(got);
		goto out;
	}
	freemsg(got);

	/* T_UNITDATA_REQ to 127.0.0.1:port with payload "udp/v1". */
	static const char payload[] = "udp/v1";
	{
		mblk_t *ctl = allocb(sizeof(struct t_unitdata_req), 0);
		if (!ctl) goto out;
		ctl->b_datap->db_type = M_PROTO;
		struct t_unitdata_req *r =
		    (struct t_unitdata_req *)ctl->b_wptr;
		r->prim     = T_UNITDATA_REQ;
		r->_pad[0]  = r->_pad[1] = r->_pad[2] = 0;
		r->dst_ip   = 0x7f000001u;
		r->dst_port = port;
		r->_pad2    = 0;
		ctl->b_wptr += sizeof(*r);

		mblk_t *data = allocb(sizeof(payload) - 1, 0);
		if (!data) { freemsg(ctl); goto out; }
		kmemcpy(data->b_wptr, payload, sizeof(payload) - 1);
		data->b_wptr += sizeof(payload) - 1;
		ctl->b_cont = data;

		putnext(sd->sd_wq, ctl);
	}

	/* Drain T_UNITDATA_IND. */
	got = NULL;
	for (int i = 0; i < 64 && !got; i++) {
		got = getq(sd->sd_rq);
		if (!got) kthread_yield();
	}
	if (!got) goto out;
	if (got->b_datap->db_type != M_PROTO
	    || got->b_rptr[0] != T_UNITDATA_IND
	    || !got->b_cont) {
		freemsg(got);
		goto out;
	}
	struct t_unitdata_ind *ind = (struct t_unitdata_ind *)got->b_rptr;
	mblk_t *body = got->b_cont;
	if (ind->src_port != port) { freemsg(got); goto out; }
	if ((body->b_wptr - body->b_rptr) != (int)(sizeof(payload) - 1)) {
		freemsg(got);
		goto out;
	}
	if (kmemcmp(body->b_rptr, payload, sizeof(payload) - 1) != 0) {
		freemsg(got);
		goto out;
	}
	freemsg(got);
	rc = 0;

out:
	stream_destroy_kernel(sd);
	return rc;
}

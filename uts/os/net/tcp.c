/*
 * uts/os/net/tcp.c -- TCP as a STREAMS module above IP.
 *
 * Phase T1a: skeleton with bind/unbind through IP.
 * Phase T1b: 3-way handshake.  Active open (T_CONN_REQ -> SYN ->
 *            SYN-ACK -> ACK -> ESTABLISHED + T_CONN_CON), plus the
 *            symmetric passive path (BOUND -> SYN_RECEIVED ->
 *            ESTABLISHED) so we can self-test the full handshake
 *            against ourselves over lo0.  No accept queue yet --
 *            a "bound" tcb acts as a single-connection listener
 *            and morphs into the established connection on first
 *            SYN.  T1d will split LISTEN from accepted connections
 *            via a proper accept queue.
 * Phase T1c: data send/recv (T_TCP_DATA_REQ / _IND) with in-order
 *            sequence/ack tracking.  Out-of-order segments are
 *            dropped; T1f's reorder buffer + retransmit lights up
 *            once a peer that actually drops things is in scope.
 *            Every received segment is ACK'd inline (no delayed
 *            ACK); the piggyback path is exercised when both sides
 *            send data simultaneously.
 *
 * Inbound segment flow:
 *   ip_demux -> tcp_rq_putp (one M_PROTO+M_DATA per fanout)
 *     filter: dst_port == s->local_port  (otherwise drop)
 *     dispatch by s->state:
 *       SYN_SENT     + SYN+ACK  -> send ACK, ESTABLISHED, T_CONN_CON
 *       BOUND        + SYN      -> learn (remote_ip,port), send SYN+ACK,
 *                                  SYN_RECEIVED
 *       SYN_RECEIVED + ACK      -> ESTABLISHED, T_CONN_CON
 *       ESTABLISHED  + ACK      -> advance snd_una if seg_ack newer
 *       ESTABLISHED  + data     -> if seg_seq==rcv_nxt and remote
 *                                  matches: copy bytes, advance
 *                                  rcv_nxt, send ACK, T_DATA_IND up
 *
 * The bind table key is (local_port << 16) | remote_port.  T1a/T1b/T1c
 * use rport=0; T1d's accept queue lets us refine the key for connected
 * sockets so IP demux delivers segments precisely instead of
 * multicasting to every TCP endpoint that has to filter at its rput.
 *
 * Per-flow checksums use the standard TCP/UDP pseudo-header
 * (src_ip, dst_ip, 0, proto, tcp_len) prepended to the segment.
 * Source IP is whatever IP would put on the wire to reach dst --
 * `ip_route_src(dst)` answers that.
 */

#include <stdint.h>

#include "kappara/core/kmem.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"
#include "kappara/io/cdevsw.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/net/ip.h"
#include "kappara/net/tcp.h"
#include "kappara/proc/sched.h"

/* ---- Per-connection state (TCB) ---------------------------------- */

enum tcp_state {
	TCPS_CLOSED        = 0,
	TCPS_BOUND         = 1,	/* local addr/port assigned, listening implicit */
	TCPS_LISTEN        = 2,	/* T1d split LISTEN from BOUND */
	TCPS_SYN_SENT      = 3,
	TCPS_SYN_RECEIVED  = 4,
	TCPS_ESTABLISHED   = 5,
	TCPS_FIN_WAIT_1    = 6,	/* T1e */
	TCPS_FIN_WAIT_2    = 7,
	TCPS_CLOSE_WAIT    = 8,
	TCPS_LAST_ACK      = 9,
};

struct tcp_tcb {
	enum tcp_state  state;

	/* Local endpoint (T_BIND_REQ). */
	uint16_t   local_port;
	int        bound;	/* IP_T_BIND_ACK received */

	/* Remote endpoint (T_CONN_REQ or learned from inbound SYN). */
	uint32_t   remote_ip;
	uint16_t   remote_port;

	/* Sequence-number bookkeeping (RFC 793 section 3.2). */
	uint32_t   snd_una;	/* oldest unacknowledged seq */
	uint32_t   snd_nxt;	/* next seq to send          */
	uint32_t   snd_iss;	/* our initial seq           */
	uint32_t   rcv_nxt;	/* next seq we expect to recv */
	uint32_t   rcv_irs;	/* peer's initial seq        */

	/* T1d listener state.  LISTEN + SYN sets listen_syn_pending=1
	 * and stores peer info; the SYN-ACK only goes out once the user
	 * sends T_CONN_RES (real TPI semantics where the user gets to
	 * inspect / accept / reject the incoming connection request).
	 * For T1d single-listener-one-conn there's only one pending
	 * slot; T1d.5 grows this into a real accept queue. */
	int        listen_syn_pending;
};

/* Global ISS counter.  Real TCP picks ISS via a clock-keyed random
 * function (RFC 6528) to avoid sequence-number prediction.  We just
 * monotonically increment; trivial to predict but the network is a
 * tiny local segment of point-to-point links and lo0. */
static uint32_t tcp_iss_next = 0x12345;

static uint32_t alloc_iss(void)
{
	uint32_t v = tcp_iss_next;
	tcp_iss_next += 0x10000;
	return v;
}

static uint32_t tcp_bind_key(uint16_t lport, uint16_t rport)
{
	return ((uint32_t)lport << 16) | (uint32_t)rport;
}

static uint16_t tcp_ephemeral_next = 49152;

static uint16_t alloc_ephemeral_port(void)
{
	uint16_t p = tcp_ephemeral_next++;
	if (tcp_ephemeral_next == 0) tcp_ephemeral_next = 49152;
	return p;
}

/* ---- TCP pseudo-header checksum --------------------------------- */
/*
 * RFC 793: checksum covers a 12-byte pseudo-header (src_ip, dst_ip,
 * zero, proto, length) plus the TCP segment.  Pad the segment to
 * an even number of bytes for the one's-complement sum.
 *
 * We reuse ip_checksum's RFC 1071 algorithm by serialising the
 * pseudo-header into a stack buffer and running ip_checksum across
 * pseudo-header + segment via a small "sum two ranges" loop.
 */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const void *seg, unsigned seg_len)
{
	uint8_t ph[12];
	ph[0]  = (src_ip >> 24) & 0xff;
	ph[1]  = (src_ip >> 16) & 0xff;
	ph[2]  = (src_ip >>  8) & 0xff;
	ph[3]  = (src_ip      ) & 0xff;
	ph[4]  = (dst_ip >> 24) & 0xff;
	ph[5]  = (dst_ip >> 16) & 0xff;
	ph[6]  = (dst_ip >>  8) & 0xff;
	ph[7]  = (dst_ip      ) & 0xff;
	ph[8]  = 0;
	ph[9]  = IPPROTO_TCP;
	ph[10] = (seg_len >> 8) & 0xff;
	ph[11] =  seg_len       & 0xff;

	/* One's-complement sum over ph + segment.  Pad the segment
	 * with a trailing zero byte if odd-length. */
	uint32_t sum = 0;
	const uint8_t *p = ph;
	for (unsigned i = 0; i < 12; i += 2)
		sum += (uint16_t)((p[i] << 8) | p[i+1]);

	p = (const uint8_t *)seg;
	unsigned i = 0;
	for (; i + 1 < seg_len; i += 2)
		sum += (uint16_t)((p[i] << 8) | p[i+1]);
	if (i < seg_len)
		sum += (uint32_t)(p[i] << 8);

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

/* ---- IP bind helpers --------------------------------------------- */

static void send_ip_bind(queue_t *rq, uint8_t prim, uint32_t key)
{
	mblk_t *mp = allocb(sizeof(struct ip_bind_meta), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct ip_bind_meta *m = (struct ip_bind_meta *)mp->b_wptr;
	m->prim   = prim;
	m->proto  = IPPROTO_TCP;
	m->_pad[0] = m->_pad[1] = 0;
	m->key    = key;
	mp->b_wptr += sizeof(*m);
	putnext(WR(rq), mp);
}

/* ---- TPI replies up the stream ----------------------------------- */

static void reply_bind_ack(queue_t *rq, uint16_t port)
{
	mblk_t *mp = allocb(sizeof(struct t_tcp_bind_ack), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct t_tcp_bind_ack *a = (struct t_tcp_bind_ack *)mp->b_wptr;
	a->prim    = T_TCP_BIND_ACK;
	a->_pad[0] = 0;
	a->port    = port;
	mp->b_wptr += sizeof(*a);
	putnext(rq, mp);
}

static void reply_bind_nak(queue_t *rq, uint8_t reason)
{
	mblk_t *mp = allocb(sizeof(struct t_tcp_bind_nak), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct t_tcp_bind_nak *n = (struct t_tcp_bind_nak *)mp->b_wptr;
	n->prim    = T_TCP_BIND_NAK;
	n->reason  = reason;
	n->_pad[0] = n->_pad[1] = 0;
	mp->b_wptr += sizeof(*n);
	putnext(rq, mp);
}

static void reply_discon_ind(queue_t *rq, uint8_t reason)
{
	mblk_t *mp = allocb(sizeof(struct t_tcp_discon_ind), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct t_tcp_discon_ind *d = (struct t_tcp_discon_ind *)mp->b_wptr;
	d->prim    = T_TCP_DISCON_IND;
	d->reason  = reason;
	d->_pad[0] = d->_pad[1] = 0;
	mp->b_wptr += sizeof(*d);
	putnext(rq, mp);
}

static void reply_conn_con(queue_t *rq, uint32_t dst_ip, uint16_t dst_port)
{
	mblk_t *mp = allocb(sizeof(struct t_tcp_conn_con), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct t_tcp_conn_con *c = (struct t_tcp_conn_con *)mp->b_wptr;
	c->prim     = T_TCP_CONN_CON;
	c->_pad[0]  = c->_pad[1] = c->_pad[2] = 0;
	c->dst_ip   = dst_ip;
	c->dst_port = dst_port;
	c->_pad2    = 0;
	mp->b_wptr += sizeof(*c);
	putnext(rq, mp);
}

/* Build T_CONN_IND ("an inbound connection is pending; send
 * T_CONN_RES to accept it") and deliver UP.  Carries the peer's
 * (src_ip, src_port) so the listener's user can inspect before
 * accepting. */
static void reply_conn_ind(queue_t *rq, uint32_t src_ip, uint16_t src_port)
{
	mblk_t *mp = allocb(sizeof(struct t_tcp_conn_ind), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct t_tcp_conn_ind *i = (struct t_tcp_conn_ind *)mp->b_wptr;
	i->prim     = T_TCP_CONN_IND;
	i->_pad[0]  = i->_pad[1] = i->_pad[2] = 0;
	i->src_ip   = src_ip;
	i->src_port = src_port;
	i->_pad2    = 0;
	mp->b_wptr += sizeof(*i);
	putnext(rq, mp);
}

/* Build T_DATA_IND M_PROTO + M_DATA(payload) and deliver UP to the
 * stream head so the user's getmsg can return both. */
static void reply_data_ind(queue_t *rq, mblk_t *payload)
{
	mblk_t *ind = allocb(sizeof(struct t_tcp_data_ind), 0);
	if (!ind) {
		freemsg(payload);
		return;
	}
	ind->b_datap->db_type = M_PROTO;
	struct t_tcp_data_ind *d = (struct t_tcp_data_ind *)ind->b_wptr;
	d->prim    = T_TCP_DATA_IND;
	d->_pad[0] = d->_pad[1] = d->_pad[2] = 0;
	ind->b_wptr += sizeof(*d);
	ind->b_cont  = payload;
	putnext(rq, ind);
}

/* ---- Segment send ------------------------------------------------ */
/*
 * Build a TCP segment (header + optional payload), wrap in M_DATA
 * with IP headroom reserved, and hand off to ip_send.  Payload, if
 * any, is taken from `payload` -- we walk its b_cont chain copying
 * bytes into the contiguous segment buffer (good enough at our
 * scale; zero-copy chains would need ip_wput to handle them).
 * Caller relinquishes ownership of `payload`.
 *
 * SYN and FIN each consume one sequence-number slot (RFC 793 3.2);
 * data consumes its byte count.  All three advance snd_nxt before
 * the segment goes out so the loopback recursion observes the
 * post-send state.
 */
static int tcp_send_segment(struct tcp_tcb *s, uint8_t flags,
                            mblk_t *payload)
{
	unsigned plen = payload ? (unsigned)msgdsize(payload) : 0;
	uint32_t src_ip = ip_route_src(s->remote_ip);
	if (src_ip == 0) {
		if (payload) freemsg(payload);
		kprintf("tcp: no route to %u.%u.%u.%u\n",
			(s->remote_ip >> 24) & 0xff,
			(s->remote_ip >> 16) & 0xff,
			(s->remote_ip >>  8) & 0xff,
			 s->remote_ip        & 0xff);
		return -1;
	}

	mblk_t *mp = allocb(IP_HDR_LEN + TCP_HDR_LEN_MIN + plen, 0);
	if (!mp) {
		if (payload) freemsg(payload);
		return -1;
	}
	mp->b_rptr += IP_HDR_LEN;
	mp->b_wptr  = mp->b_rptr;

	struct tcp_hdr *h = (struct tcp_hdr *)mp->b_wptr;
	h->src_port = htons16(s->local_port);
	h->dst_port = htons16(s->remote_port);
	h->seq      = htonl32(s->snd_nxt);
	h->ack      = htonl32(s->rcv_nxt);
	h->data_off = (TCP_HDR_LEN_MIN / 4) << 4;
	h->flags    = flags;
	h->window   = htons16(8192);
	h->checksum = 0;
	h->urg_ptr  = 0;
	mp->b_wptr += TCP_HDR_LEN_MIN;

	if (payload) {
		for (mblk_t *m = payload; m; m = m->b_cont) {
			unsigned n = (unsigned)(m->b_wptr - m->b_rptr);
			if (n == 0) continue;
			kmemcpy(mp->b_wptr, m->b_rptr, n);
			mp->b_wptr += n;
		}
		freemsg(payload);
	}

	unsigned seg_len = TCP_HDR_LEN_MIN + plen;
	uint16_t cs = tcp_checksum(src_ip, s->remote_ip, h, seg_len);
	h->checksum = htons16(cs);

	if (flags & TCP_FLAG_SYN) s->snd_nxt += 1;
	if (flags & TCP_FLAG_FIN) s->snd_nxt += 1;
	s->snd_nxt += plen;

	return ip_send(s->remote_ip, IPPROTO_TCP, mp);
}

/* ---- Inbound segment processing --------------------------------- */

static void tcp_input_segment(queue_t *q, struct tcp_tcb *s,
                              uint32_t src_ip, uint32_t dst_ip,
                              mblk_t *body)
{
	(void)dst_ip;
	unsigned len = (unsigned)(body->b_wptr - body->b_rptr);
	if (len < TCP_HDR_LEN_MIN) {
		freemsg(body);
		return;
	}
	struct tcp_hdr *h = (struct tcp_hdr *)body->b_rptr;
	unsigned hl = (unsigned)((h->data_off >> 4) * 4);
	if (hl < TCP_HDR_LEN_MIN || hl > len) {
		freemsg(body);
		return;
	}

	/* Verify checksum -- if peer set it.  Our outgoing always sets;
	 * we accept zero as "checksum not computed" for robustness. */
	uint16_t pkt_cs = h->checksum;
	if (pkt_cs != 0) {
		uint16_t saved = h->checksum;
		h->checksum = 0;
		uint16_t want = tcp_checksum(src_ip, dst_ip, h, len);
		h->checksum = saved;
		if (want != ntohs16(saved)) {
			kprintf("tcp: checksum fail\n");
			freemsg(body);
			return;
		}
	}

	uint16_t sport = ntohs16(h->src_port);
	uint16_t dport = ntohs16(h->dst_port);
	uint32_t seg_seq = ntohl32(h->seq);
	uint32_t seg_ack = ntohl32(h->ack);
	uint8_t  flags   = h->flags;

	/* Port filter -- IP multicasts to all proto=TCP binds; each
	 * endpoint accepts only its own. */
	if (dport != s->local_port) {
		freemsg(body);
		return;
	}

	switch (s->state) {
	case TCPS_SYN_SENT:
		/* Expecting SYN+ACK (passive simultaneous-open simplified).
		 *
		 * State transitions are updated BEFORE tcp_send_segment so
		 * that lo0's synchronous loopback (which re-enters
		 * tcp_rq_putp inside the send call) sees the post-transition
		 * state.  Otherwise the recursive callback observes stale
		 * state and drops the response that would've completed the
		 * handshake. */
		if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK))
		      == (TCP_FLAG_SYN | TCP_FLAG_ACK)
		 && seg_ack == s->snd_nxt
		 && src_ip == s->remote_ip
		 && sport  == s->remote_port) {
			s->rcv_irs = seg_seq;
			s->rcv_nxt = seg_seq + 1;
			s->snd_una = seg_ack;
			s->state   = TCPS_ESTABLISHED;
			(void)tcp_send_segment(s, TCP_FLAG_ACK, NULL);
			reply_conn_con(q, src_ip, sport);
		}
		break;

	case TCPS_BOUND:
		/* T1d: a bound-but-not-listening socket silently drops
		 * SYNs.  User must explicitly T_LISTEN_REQ to accept
		 * incoming connections. */
		break;

	case TCPS_LISTEN:
		/* T1d: record peer + seq numbers, fire T_CONN_IND, but do
		 * NOT send SYN-ACK yet.  The user must send T_CONN_RES to
		 * accept (or T_DISCON_REQ to refuse, in T1e).  Single-slot
		 * pending model: a second SYN while one is already pending
		 * is dropped.  T1d.5's accept queue will replace this. */
		if ((flags & TCP_FLAG_SYN) && !s->listen_syn_pending) {
			s->remote_ip          = src_ip;
			s->remote_port        = sport;
			s->rcv_irs            = seg_seq;
			s->rcv_nxt            = seg_seq + 1;
			s->listen_syn_pending = 1;
			reply_conn_ind(q, src_ip, sport);
		}
		break;

	case TCPS_SYN_RECEIVED:
		/* Expecting ACK for our SYN. */
		if ((flags & TCP_FLAG_ACK)
		 && seg_ack == s->snd_nxt
		 && src_ip == s->remote_ip
		 && sport  == s->remote_port) {
			s->snd_una = seg_ack;
			s->state   = TCPS_ESTABLISHED;
			reply_conn_con(q, src_ip, sport);
		}
		break;

	case TCPS_ESTABLISHED: {
		/* Verify the segment belongs to this connection.  T1c
		 * doesn't bother with ack-acceptability checks
		 * (snd_una < ack <= snd_nxt) since we don't retransmit
		 * yet; T1f's retransmit timer needs those. */
		if (src_ip != s->remote_ip || sport != s->remote_port)
			break;

		/* Piggybacked ACK: advance snd_una if seg_ack is newer.
		 * Compare via int32 subtraction so wraparound is handled
		 * the canonical RFC 793 way. */
		if (flags & TCP_FLAG_ACK) {
			if ((int32_t)(seg_ack - s->snd_una) > 0)
				s->snd_una = seg_ack;
		}

		/* Data payload: in-order only.  Out-of-order goes to the
		 * reorder buffer in T1f.  RST and FIN come in T1e. */
		unsigned data_len = len - hl;
		if (data_len > 0 && seg_seq == s->rcv_nxt) {
			mblk_t *data = allocb(data_len, 0);
			if (data) {
				kmemcpy(data->b_wptr,
				        body->b_rptr + hl, data_len);
				data->b_wptr += data_len;
				/* Advance rcv_nxt BEFORE the ACK send so
				 * lo0's synchronous loopback sees the
				 * post-receive state.  (Same trap T1b
				 * hit; see the file header.) */
				s->rcv_nxt += data_len;
				(void)tcp_send_segment(s, TCP_FLAG_ACK, NULL);
				reply_data_ind(q, data);
			}
		}
		break;
	}

	default:
		break;
	}
	freemsg(body);
}

/* ---- qopen / qclose ---------------------------------------------- */

static int tcp_qopen(queue_t *rq)
{
	struct tcp_tcb *s = kmalloc(sizeof(*s));
	if (!s) return -1;
	kmemset(s, 0, sizeof(*s));
	s->state = TCPS_CLOSED;
	rq->q_ptr     = s;
	WR(rq)->q_ptr = s;
	return 0;
}

static int tcp_qclose(queue_t *rq)
{
	struct tcp_tcb *s = rq->q_ptr;
	if (!s) return 0;
	if (s->bound) {
		uint32_t key = tcp_bind_key(s->local_port, 0);
		send_ip_bind(rq, IP_T_UNBIND_REQ, key);
	}
	rq->q_ptr     = NULL;
	WR(rq)->q_ptr = NULL;
	kfree(s);
	return 0;
}

/* ---- Write side -------------------------------------------------- */

static int handle_bind_req(queue_t *q, struct tcp_tcb *s,
                           const struct t_tcp_bind_req *req)
{
	if (s->bound) {
		reply_bind_nak(OTHERQ(q), 1);
		return 0;
	}
	uint16_t port = req->port ? req->port : alloc_ephemeral_port();
	s->local_port = port;
	s->state      = TCPS_BOUND;
	send_ip_bind(OTHERQ(q), IP_T_BIND_REQ, tcp_bind_key(port, 0));
	return 0;
}

static int handle_conn_req(queue_t *q, struct tcp_tcb *s,
                           const struct t_tcp_conn_req *req)
{
	if (s->state != TCPS_BOUND) {
		reply_discon_ind(OTHERQ(q), TCP_DISCON_REASON_PROTOERR);
		return 0;
	}
	s->remote_ip   = req->dst_ip;
	s->remote_port = req->dst_port;
	s->snd_iss     = alloc_iss();
	s->snd_una     = s->snd_iss;
	s->snd_nxt     = s->snd_iss;
	s->state       = TCPS_SYN_SENT;
	(void)tcp_send_segment(s, TCP_FLAG_SYN, NULL);
	return 0;
}

static int handle_listen_req(queue_t *q, struct tcp_tcb *s)
{
	(void)q;
	if (s->state != TCPS_BOUND) {
		reply_discon_ind(OTHERQ(q), TCP_DISCON_REASON_PROTOERR);
		return 0;
	}
	s->state = TCPS_LISTEN;
	return 0;
}

static int handle_conn_res(queue_t *q, struct tcp_tcb *s)
{
	if (s->state != TCPS_LISTEN || !s->listen_syn_pending) {
		reply_discon_ind(OTHERQ(q), TCP_DISCON_REASON_PROTOERR);
		return 0;
	}
	/* User has accepted the pending SYN.  Send SYN-ACK now and
	 * transition to SYN_RECEIVED.  The eventual ACK transitions
	 * us to ESTABLISHED + T_CONN_CON via the existing
	 * SYN_RECEIVED case in tcp_input_segment. */
	s->snd_iss            = alloc_iss();
	s->snd_una            = s->snd_iss;
	s->snd_nxt            = s->snd_iss;
	s->listen_syn_pending = 0;
	s->state              = TCPS_SYN_RECEIVED;
	(void)tcp_send_segment(s, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL);
	return 0;
}

static int handle_data_req(queue_t *q, struct tcp_tcb *s, mblk_t *payload)
{
	if (s->state != TCPS_ESTABLISHED) {
		if (payload) freemsg(payload);
		reply_discon_ind(OTHERQ(q), TCP_DISCON_REASON_PROTOERR);
		return -1;
	}
	if (!payload || msgdsize(payload) == 0) {
		if (payload) freemsg(payload);
		return 0;	/* zero-byte data: nothing to send */
	}
	return tcp_send_segment(s, TCP_FLAG_ACK | TCP_FLAG_PSH, payload);
}

static int tcp_wq_putp(queue_t *q, mblk_t *mp)
{
	struct tcp_tcb *s = q->q_ptr;
	if (!s || !mp) {
		freemsg(mp);
		return -1;
	}
	if (mp->b_datap->db_type != M_PROTO
	    || (mp->b_wptr - mp->b_rptr) < 1) {
		freemsg(mp);
		return -1;
	}
	uint8_t prim = mp->b_rptr[0];

	int rc = -1;
	if (prim == T_TCP_BIND_REQ
	    && (mp->b_wptr - mp->b_rptr)
	       >= (int)sizeof(struct t_tcp_bind_req)) {
		rc = handle_bind_req(q, s,
		                     (const struct t_tcp_bind_req *)mp->b_rptr);
		freemsg(mp);
	} else if (prim == T_TCP_CONN_REQ
	    && (mp->b_wptr - mp->b_rptr)
	       >= (int)sizeof(struct t_tcp_conn_req)) {
		rc = handle_conn_req(q, s,
		                     (const struct t_tcp_conn_req *)mp->b_rptr);
		freemsg(mp);
	} else if (prim == T_TCP_DATA_REQ) {
		mblk_t *payload = mp->b_cont;
		mp->b_cont = NULL;
		freemsg(mp);
		rc = handle_data_req(q, s, payload);
	} else if (prim == T_TCP_LISTEN_REQ) {
		rc = handle_listen_req(q, s);
		freemsg(mp);
	} else if (prim == T_TCP_CONN_RES) {
		rc = handle_conn_res(q, s);
		freemsg(mp);
	} else if (prim == T_TCP_ORDREL_REQ
	        || prim == T_TCP_DISCON_REQ) {
		/* Primitives that exist in the ABI but aren't wired up
		 * yet at this phase. */
		reply_discon_ind(OTHERQ(q), TCP_DISCON_REASON_NOTSUP);
		freemsg(mp);
		rc = 0;
	} else {
		freemsg(mp);
	}
	return rc;
}

/* ---- Read side: IP bind acks + inbound segments ----------------- */

static void handle_ip_ack(queue_t *q, struct tcp_tcb *s, mblk_t *mp)
{
	const struct ip_bind_meta *m = (const struct ip_bind_meta *)mp->b_rptr;
	uint8_t  prim = m->prim;
	uint32_t key  = m->key;
	uint16_t lp   = (uint16_t)(key >> 16);
	freemsg(mp);

	if (prim == IP_T_BIND_ACK) {
		s->bound = 1;
		reply_bind_ack(q, lp);
	} else if (prim == IP_T_BIND_NAK) {
		s->bound      = 0;
		s->local_port = 0;
		s->state      = TCPS_CLOSED;
		reply_bind_nak(q, 1);
	}
}

static int tcp_rq_putp(queue_t *q, mblk_t *mp)
{
	struct tcp_tcb *s = q->q_ptr;
	if (!s || !mp) {
		freemsg(mp);
		return -1;
	}
	if (mp->b_datap->db_type != M_PROTO
	    || (mp->b_wptr - mp->b_rptr) < 1) {
		freemsg(mp);
		return 0;
	}
	uint8_t prim = mp->b_rptr[0];

	if (prim == IP_T_BIND_ACK || prim == IP_T_BIND_NAK
	 || prim == IP_T_UNBIND_ACK) {
		handle_ip_ack(q, s, mp);
		return 0;
	}

	if (prim == IP_T_UNITDATA_IND
	    && (mp->b_wptr - mp->b_rptr)
	       >= (int)sizeof(struct ip_unitdata_ind)
	    && mp->b_cont) {
		const struct ip_unitdata_ind *ind =
		    (const struct ip_unitdata_ind *)mp->b_rptr;
		uint32_t src_ip = ind->src_ip;
		uint32_t dst_ip = ind->dst_ip;
		mblk_t *body = mp->b_cont;
		mp->b_cont = NULL;
		freeb(mp);
		tcp_input_segment(q, s, src_ip, dst_ip, body);
		return 0;
	}

	freemsg(mp);
	return 0;
}

/* ---- Module wiring ----------------------------------------------- */

static struct module_info tcp_minfo = {
	.mi_idnum  = 1000,
	.mi_idname = "tcp",
	.mi_minpsz = 0,
	.mi_maxpsz = 65535,
	.mi_hiwat  = 32768,
	.mi_lowat  = 16384,
};

static struct qinit tcp_rinit = {
	.qi_putp  = tcp_rq_putp,
	.qi_qopen = tcp_qopen,
	.qi_qclose= tcp_qclose,
	.qi_minfo = &tcp_minfo,
};
static struct qinit tcp_winit = {
	.qi_putp  = tcp_wq_putp,
	.qi_minfo = &tcp_minfo,
};
struct streamtab tcp_streamtab = {
	.st_rdinit = &tcp_rinit,
	.st_wrinit = &tcp_winit,
};

void tcp_module_init(void)
{
	streams_register("tcp", &tcp_streamtab);
	cdev_register(CDEV_MAJ_TCP, "tcp", &ip_streamtab);
}

/* ---- In-kernel selftest ------------------------------------------ */
/*
 * T1b: full 3-way handshake to ourselves over lo0.
 *   - Stream L: T_TCP_BIND_REQ port=12345, then sits in BOUND
 *     (implicit listener).
 *   - Stream C: T_TCP_BIND_REQ port=0 (ephemeral), then T_TCP_CONN_REQ
 *     to (127.0.0.1, 12345).  Sends SYN.
 *   - L receives the SYN, transitions BOUND -> SYN_RECEIVED, sends
 *     SYN-ACK.
 *   - C receives SYN-ACK, transitions SYN_SENT -> ESTABLISHED, sends
 *     ACK, fires T_TCP_CONN_CON up.
 *   - L receives ACK, transitions SYN_RECEIVED -> ESTABLISHED, fires
 *     T_TCP_CONN_CON up.
 *
 * Selftest reads T_TCP_CONN_CON from both head queues and passes if
 * both arrived.
 */

static int drain_for_prim(struct stdata *sd, uint8_t want_prim)
{
	for (int i = 0; i < 64; i++) {
		mblk_t *mp = getq(sd->sd_rq);
		if (!mp) { kthread_yield(); continue; }
		uint8_t prim = (mp->b_wptr - mp->b_rptr) >= 1
		             ? mp->b_rptr[0] : 0;
		freemsg(mp);
		if (prim == want_prim) return 0;
	}
	return -1;
}

/* Drain T_DATA_IND and verify its M_DATA payload matches `expect`. */
static int drain_data_ind(struct stdata *sd, const char *expect,
                          unsigned elen)
{
	for (int i = 0; i < 64; i++) {
		mblk_t *mp = getq(sd->sd_rq);
		if (!mp) { kthread_yield(); continue; }
		uint8_t prim = (mp->b_wptr - mp->b_rptr) >= 1
		             ? mp->b_rptr[0] : 0;
		if (prim != T_TCP_DATA_IND || !mp->b_cont) {
			freemsg(mp);
			continue;
		}
		mblk_t *body = mp->b_cont;
		int ok = ((body->b_wptr - body->b_rptr) == (int)elen)
		      && kmemcmp(body->b_rptr, expect, elen) == 0;
		freemsg(mp);
		return ok ? 0 : -1;
	}
	return -1;
}

static int send_data_req(struct stdata *sd, const char *buf, unsigned blen)
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

static int send_bind_req(struct stdata *sd, uint16_t port)
{
	mblk_t *mp = allocb(sizeof(struct t_tcp_bind_req), 0);
	if (!mp) return -1;
	mp->b_datap->db_type = M_PROTO;
	struct t_tcp_bind_req *r = (struct t_tcp_bind_req *)mp->b_wptr;
	r->prim    = T_TCP_BIND_REQ;
	r->_pad[0] = 0;
	r->port    = port;
	mp->b_wptr += sizeof(*r);
	putnext(sd->sd_wq, mp);
	return drain_for_prim(sd, T_TCP_BIND_ACK);
}

int tcp_selftest_run(void)
{
	struct stdata *L = NULL, *C = NULL;
	int rc = -1;

	L = stream_build_kernel(&ip_streamtab, "tcp_listen", 0);
	C = stream_build_kernel(&ip_streamtab, "tcp_client", 0);
	if (!L || !C) goto out;
	if (stream_push_kernel(L, "tcp") < 0) goto out;
	if (stream_push_kernel(C, "tcp") < 0) goto out;

	/* Bind both. */
	if (send_bind_req(L, 12345) < 0) goto out;
	if (send_bind_req(C, 0) < 0)     goto out;

	/* L: T_TCP_LISTEN_REQ to start accepting SYNs. */
	{
		mblk_t *mp = allocb(sizeof(struct t_tcp_listen_req), 0);
		if (!mp) goto out;
		mp->b_datap->db_type = M_PROTO;
		struct t_tcp_listen_req *r =
		    (struct t_tcp_listen_req *)mp->b_wptr;
		r->prim    = T_TCP_LISTEN_REQ;
		r->_pad[0] = r->_pad[1] = r->_pad[2] = 0;
		mp->b_wptr += sizeof(*r);
		putnext(L->sd_wq, mp);
	}

	/* Client: T_TCP_CONN_REQ to 127.0.0.1:12345.  This sends SYN.
	 * L will receive it, fire T_CONN_IND, and wait for T_CONN_RES. */
	{
		mblk_t *mp = allocb(sizeof(struct t_tcp_conn_req), 0);
		if (!mp) goto out;
		mp->b_datap->db_type = M_PROTO;
		struct t_tcp_conn_req *r = (struct t_tcp_conn_req *)mp->b_wptr;
		r->prim    = T_TCP_CONN_REQ;
		r->_pad[0] = r->_pad[1] = r->_pad[2] = 0;
		r->dst_ip   = 0x7f000001u;
		r->dst_port = 12345;
		r->_pad2    = 0;
		mp->b_wptr += sizeof(*r);
		putnext(C->sd_wq, mp);
	}

	/* L should now see T_CONN_IND for the pending connection. */
	if (drain_for_prim(L, T_TCP_CONN_IND) < 0) goto out;

	/* L: T_TCP_CONN_RES to accept.  This sends SYN-ACK which C
	 * processes inline, ACKs, transitions to ESTABLISHED + T_CONN_CON,
	 * and that ACK transitions L to ESTABLISHED + T_CONN_CON. */
	{
		mblk_t *mp = allocb(sizeof(struct t_tcp_conn_res), 0);
		if (!mp) goto out;
		mp->b_datap->db_type = M_PROTO;
		struct t_tcp_conn_res *r =
		    (struct t_tcp_conn_res *)mp->b_wptr;
		r->prim    = T_TCP_CONN_RES;
		r->_pad[0] = r->_pad[1] = r->_pad[2] = 0;
		mp->b_wptr += sizeof(*r);
		putnext(L->sd_wq, mp);
	}

	if (drain_for_prim(C, T_TCP_CONN_CON) < 0) goto out;
	if (drain_for_prim(L, T_TCP_CONN_CON) < 0) goto out;

	/* T1c: bidirectional data exchange. */
	static const char msg_c2l[] = "hello-from-client";
	static const char msg_l2c[] = "and-from-listener";

	if (send_data_req(C, msg_c2l, sizeof(msg_c2l) - 1) < 0) goto out;
	if (drain_data_ind(L, msg_c2l, sizeof(msg_c2l) - 1) < 0) goto out;

	if (send_data_req(L, msg_l2c, sizeof(msg_l2c) - 1) < 0) goto out;
	if (drain_data_ind(C, msg_l2c, sizeof(msg_l2c) - 1) < 0) goto out;

	rc = 0;

out:
	if (L) stream_destroy_kernel(L);
	if (C) stream_destroy_kernel(C);
	return rc;
}

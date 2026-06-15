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
#include "kappara/fs/vfs.h"
#include "kappara/io/cdevsw.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/net/ip.h"
#include "kappara/net/tcp.h"
#include "kappara/proc/sched.h"

/* ---- Per-connection state (TCB) ---------------------------------- */

#define TCP_LISTEN_BACKLOG	8	/* pending-SYN ring depth on a LISTEN */

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

	/* T1d.5 multi-accept backlog.  Each arriving SYN on a LISTEN
	 * TCB enqueues here + fires T_CONN_IND; each T_CONN_RES pops
	 * the head, moves it into the user-supplied responding fd's
	 * TCB, and sends SYN-ACK from THAT TCB.  The listener stays in
	 * LISTEN; child TCBs filter inbound segments by 4-tuple and
	 * receive them via the listener's fan-out (see
	 * tcp_input_segment's LISTEN case). */
	struct {
		uint32_t remote_ip;
		uint16_t remote_port;
		uint32_t irs;	/* peer's initial seq */
	}          pending_q[TCP_LISTEN_BACKLOG];
	uint8_t    pending_head;
	uint8_t    pending_count;

	/* T1f retransmit + RTT.  RTO is in 10ms ticks (sched_tick is
	 * at 100Hz).  pending_flags is what we'll re-send on RTO --
	 * SYN, SYN|ACK, or ACK|FIN; data retransmit needs a send queue
	 * which T1g adds.  rto_expires=0 means timer inactive.
	 *
	 * RTT estimate (RFC 6298): srtt + 4*rttvar gives RTO, clamped
	 * to [TCP_RTO_MIN, TCP_RTO_MAX].  send_tick records when the
	 * pending segment went out so the ACK can compute the round-
	 * trip.  Updated only for non-retransmitted samples (Karn's
	 * algorithm). */
	uint32_t   rto_expires;
	uint32_t   rto_ticks;
	uint32_t   send_tick;
	uint8_t    pending_flags;
	uint8_t    retransmit_count;
	int        rtt_valid;	/* srtt/rttvar primed */
	uint32_t   srtt_ticks;
	uint32_t   rttvar_ticks;

	/* T1g send queue.  snd_buf holds unACK'd outbound bytes;
	 * snd_buf_seq is the seq number of the first byte in the
	 * buffer.  Each T_DATA_REQ appends, each ACK trims from the
	 * front.  Caps at 8 KB -- T_DATA_REQ above that returns
	 * DISCON_IND with reason=PROTOERR (no flow control yet). */
	mblk_t    *snd_buf;
	uint32_t   snd_buf_seq;

	/* Queue pointers stashed for the timer kthread (which doesn't
	 * have a queue parameter).  upper_rq = where to deliver
	 * T_DISCON_IND when we give up retransmitting. */
	queue_t   *upper_rq;
};

#define TCP_RTO_MIN	100	/* 1.0 s in 10ms ticks (RFC 6298) */
#define TCP_RTO_MAX	6000	/* 60.0 s                       */
#define TCP_RTO_INITIAL	100	/* 1.0 s before first RTT sample */
#define TCP_MAX_RETRANSMITS	5
#define TCP_SND_BUF_MAX	8192

/* ---- Tick + TCB registry --------------------------------------- */

#define TCP_MAX_TCBS	32

static struct tcp_tcb *tcp_tcb_table[TCP_MAX_TCBS];
static volatile uint32_t tcp_tick_counter;

/* Called from sched_tick at 100Hz.  Cheap counter bump; the
 * retransmit kthread polls this and does the actual work. */
void tcp_tick(void)
{
	tcp_tick_counter++;
}

static void tcp_tcb_register(struct tcp_tcb *s)
{
	for (int i = 0; i < TCP_MAX_TCBS; i++) {
		if (!tcp_tcb_table[i]) {
			tcp_tcb_table[i] = s;
			return;
		}
	}
}

static void tcp_tcb_unregister(struct tcp_tcb *s)
{
	for (int i = 0; i < TCP_MAX_TCBS; i++) {
		if (tcp_tcb_table[i] == s) {
			tcp_tcb_table[i] = NULL;
			return;
		}
	}
}

/* Find a child TCB matching the 4-tuple (lport, rip, rport).  Used by
 * the listener's fan-out: when a non-SYN segment lands on the LISTEN
 * TCB (because IP multicasts to every TCP upper), we redirect it to
 * the child that owns the connection. */
static struct tcp_tcb *tcp_find_child(uint16_t lport,
                                      uint32_t rip, uint16_t rport)
{
	for (int i = 0; i < TCP_MAX_TCBS; i++) {
		struct tcp_tcb *s = tcp_tcb_table[i];
		if (!s) continue;
		if (s->state == TCPS_CLOSED
		 || s->state == TCPS_BOUND
		 || s->state == TCPS_LISTEN
		 || s->state == TCPS_SYN_SENT) continue;
		if (s->local_port == lport
		 && s->remote_ip  == rip
		 && s->remote_port == rport)
			return s;
	}
	return NULL;
}

/* Resolve a user-passed file descriptor to its TCP TCB.  Used by
 * T_CONN_RES to attach the accepted connection to a fresh /dev/tcp
 * endpoint the user opened separately.  Returns NULL on any
 * mismatch -- bad fd, non-TCP fd, or fd points at a TCB that isn't
 * in our table.
 *
 * The TCB lives behind the topmost pushed module's q_ptr, which is
 * sd_wq->q_next (sd_wq itself is the stream HEAD's queue; the pushed
 * tcp module hangs off head.q_next per do_ipush). */
static struct tcp_tcb *tcp_tcb_from_fd(int fd)
{
	if (fd < 0 || fd >= KT_FD_MAX) return NULL;
	struct kthread *t = curthread;
	if (!t) return NULL;
	struct file *f = t->fdt[fd];
	if (!f) return NULL;
	struct stdata *sd = (struct stdata *)f->f_private;
	if (!sd || !sd->sd_wq || !sd->sd_wq->q_next) return NULL;
	struct tcp_tcb *s = sd->sd_wq->q_next->q_ptr;
	if (!s) return NULL;
	for (int i = 0; i < TCP_MAX_TCBS; i++)
		if (tcp_tcb_table[i] == s) return s;
	return NULL;
}

/* Arm / cancel the retransmit timer.  Arming records `pending_flags`
 * so the kthread knows what to re-send.  Cancelling sets
 * rto_expires=0; called when an ACK covers our outstanding seq. */
static void tcp_arm_rto(struct tcp_tcb *s, uint8_t flags)
{
	if (s->rto_ticks == 0) s->rto_ticks = TCP_RTO_INITIAL;
	s->rto_expires      = tcp_tick_counter + s->rto_ticks;
	s->send_tick        = tcp_tick_counter;
	s->pending_flags    = flags;
	s->retransmit_count = 0;
}

static void tcp_cancel_rto(struct tcp_tcb *s)
{
	s->rto_expires = 0;
}

/* RFC 6298 RTT estimator.  Karn's algorithm: don't sample on
 * retransmits (sample[retransmit_count==0] is safe). */
static void tcp_update_rtt(struct tcp_tcb *s, uint32_t sample_ticks)
{
	if (!s->rtt_valid) {
		s->srtt_ticks   = sample_ticks;
		s->rttvar_ticks = sample_ticks / 2;
		s->rtt_valid    = 1;
	} else {
		uint32_t srtt = s->srtt_ticks;
		uint32_t var  = s->rttvar_ticks;
		int32_t  d    = (int32_t)sample_ticks - (int32_t)srtt;
		uint32_t ad   = d < 0 ? (uint32_t)-d : (uint32_t)d;
		var  = (3 * var + ad) >> 2;	/* var = 0.75*var + 0.25*|err| */
		srtt = (7 * srtt + sample_ticks) >> 3;	/* srtt = 7/8 + 1/8 */
		s->srtt_ticks   = srtt;
		s->rttvar_ticks = var;
	}
	uint32_t rto = s->srtt_ticks + 4 * s->rttvar_ticks;
	if (rto < TCP_RTO_MIN) rto = TCP_RTO_MIN;
	if (rto > TCP_RTO_MAX) rto = TCP_RTO_MAX;
	s->rto_ticks = rto;
}

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

/* Build T_ORDREL_IND ("peer has done its half of the close; you
 * can read remaining buffered bytes but no more will arrive") and
 * deliver UP. */
static void reply_ordrel_ind(queue_t *rq)
{
	mblk_t *mp = allocb(sizeof(struct t_tcp_ordrel_ind), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct t_tcp_ordrel_ind *o =
	    (struct t_tcp_ordrel_ind *)mp->b_wptr;
	o->prim    = T_TCP_ORDREL_IND;
	o->_pad[0] = o->_pad[1] = o->_pad[2] = 0;
	mp->b_wptr += sizeof(*o);
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
/* Pure emit: build a segment with the given seq + flags + body and
 * push it through ip_send.  No state mutation -- snd_nxt is NOT
 * advanced, RTO is NOT armed.  Used by both the regular send wrapper
 * (which does state updates around it) and the retransmit kthread
 * (which re-sends snd_buf bytes with seq = snd_buf_seq). */
static int tcp_emit_segment(struct tcp_tcb *s, uint32_t seq, uint8_t flags,
                            const void *body, unsigned blen)
{
	uint32_t src_ip = ip_route_src(s->remote_ip);
	if (src_ip == 0) {
		kprintf("tcp: no route to %u.%u.%u.%u\n",
			(s->remote_ip >> 24) & 0xff,
			(s->remote_ip >> 16) & 0xff,
			(s->remote_ip >>  8) & 0xff,
			 s->remote_ip        & 0xff);
		return -1;
	}

	mblk_t *mp = allocb(IP_HDR_LEN + TCP_HDR_LEN_MIN + blen, 0);
	if (!mp) return -1;
	mp->b_rptr += IP_HDR_LEN;
	mp->b_wptr  = mp->b_rptr;

	struct tcp_hdr *h = (struct tcp_hdr *)mp->b_wptr;
	h->src_port = htons16(s->local_port);
	h->dst_port = htons16(s->remote_port);
	h->seq      = htonl32(seq);
	h->ack      = htonl32(s->rcv_nxt);
	h->data_off = (TCP_HDR_LEN_MIN / 4) << 4;
	h->flags    = flags;
	h->window   = htons16(8192);
	h->checksum = 0;
	h->urg_ptr  = 0;
	mp->b_wptr += TCP_HDR_LEN_MIN;

	if (blen > 0 && body) {
		kmemcpy(mp->b_wptr, body, blen);
		mp->b_wptr += blen;
	}

	unsigned seg_len = TCP_HDR_LEN_MIN + blen;
	uint16_t cs = tcp_checksum(src_ip, s->remote_ip, h, seg_len);
	h->checksum = htons16(cs);

	return ip_send(s->remote_ip, IPPROTO_TCP, mp);
}

/* Regular send wrapper: build segment from mblk payload + advance
 * snd_nxt + arm RTO when the segment carries SYN, FIN, or data.
 * Frees the payload mblk (canonical bytes for data live in snd_buf;
 * the caller passes a fresh copy of just the bytes being newly
 * sent). */
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

	/* Arm RTO whenever the segment occupies sequence space: SYN, FIN,
	 * or data.  Pure ACKs don't need retransmit -- if the peer didn't
	 * see our ACK, they'll re-send their data and we'll re-ACK. */
	if ((flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) || plen > 0)
		tcp_arm_rto(s, flags);

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

	/* T1d.5 multi-accept fan-out.  IP delivers all proto=TCP
	 * segments to every bound upper, but only the listener TCB is
	 * bound at IP level -- children share its bind and are
	 * discoverable in tcp_tcb_table by their 4-tuple.  If a
	 * non-SYN segment arrives on a LISTEN TCB, redirect it to the
	 * matching child (whose state/queue handle the real work). */
	if (s->state == TCPS_LISTEN && !(flags & TCP_FLAG_SYN)) {
		struct tcp_tcb *child =
		    tcp_find_child(s->local_port, src_ip, sport);
		if (!child) {
			freemsg(body);
			return;
		}
		s = child;
		if (child->upper_rq) q = child->upper_rq;
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
			/* SYN got ACK'd: cancel RTO, sample RTT, learn IRS. */
			if (s->retransmit_count == 0)
				tcp_update_rtt(s,
				    tcp_tick_counter - s->send_tick);
			tcp_cancel_rto(s);
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
		/* T1d.5: append the SYN to the backlog ring + fire
		 * T_CONN_IND.  User sends T_CONN_RES (with a fresh fd in
		 * `responding_fd`) to accept; that pops the head pending
		 * entry into the new TCB and sends SYN-ACK from there.
		 * The listener stays in LISTEN, so concurrent inbound
		 * connections each get their own slot until the ring is
		 * full (then we silently drop -- peer will retransmit). */
		if (flags & TCP_FLAG_SYN) {
			if (s->pending_count >= TCP_LISTEN_BACKLOG) break;
			int slot = (s->pending_head + s->pending_count)
			           % TCP_LISTEN_BACKLOG;
			s->pending_q[slot].remote_ip   = src_ip;
			s->pending_q[slot].remote_port = sport;
			s->pending_q[slot].irs         = seg_seq;
			s->pending_count++;
			reply_conn_ind(q, src_ip, sport);
		}
		break;

	case TCPS_SYN_RECEIVED:
		/* Expecting ACK for our SYN. */
		if ((flags & TCP_FLAG_ACK)
		 && seg_ack == s->snd_nxt
		 && src_ip == s->remote_ip
		 && sport  == s->remote_port) {
			if (s->retransmit_count == 0)
				tcp_update_rtt(s,
				    tcp_tick_counter - s->send_tick);
			tcp_cancel_rto(s);
			s->snd_una = seg_ack;
			s->state   = TCPS_ESTABLISHED;
			reply_conn_con(q, src_ip, sport);
		}
		break;

	case TCPS_ESTABLISHED:
	case TCPS_FIN_WAIT_1:
	case TCPS_FIN_WAIT_2:
	case TCPS_CLOSE_WAIT:
	case TCPS_LAST_ACK: {
		/* Verify the segment belongs to this connection. */
		if (src_ip != s->remote_ip || sport != s->remote_port)
			break;

		/* RST in any post-handshake state aborts the connection. */
		if (flags & TCP_FLAG_RST) {
			s->state = TCPS_CLOSED;
			reply_discon_ind(q, TCP_DISCON_REASON_RESET);
			break;
		}

		/* Piggybacked ACK: advance snd_una if seg_ack is newer.
		 * Compare via int32 subtraction so wraparound is handled
		 * the canonical RFC 793 way. */
		if (flags & TCP_FLAG_ACK) {
			if ((int32_t)(seg_ack - s->snd_una) > 0) {
				uint32_t acked = seg_ack - s->snd_una;
				s->snd_una = seg_ack;
				/* T1g: trim ACK'd bytes from snd_buf.
				 * Cancel RTO when fully drained; reset on
				 * partial ACK so the next byte gets its
				 * own retransmit budget. */
				if (s->snd_buf) {
					unsigned buf_len = (unsigned)
					    (s->snd_buf->b_wptr - s->snd_buf->b_rptr);
					if (acked >= buf_len) {
						freemsg(s->snd_buf);
						s->snd_buf     = NULL;
						s->snd_buf_seq = 0;
						if (s->retransmit_count == 0)
							tcp_update_rtt(s,
							    tcp_tick_counter
							    - s->send_tick);
						tcp_cancel_rto(s);
					} else {
						s->snd_buf->b_rptr += acked;
						s->snd_buf_seq += acked;
						/* Partial ACK: reset RTO so
						 * the remaining bytes get a
						 * fresh timer. */
						s->rto_expires = tcp_tick_counter
						               + s->rto_ticks;
						s->send_tick = tcp_tick_counter;
						s->retransmit_count = 0;
					}
				}
			}
			/* State-machine effects of the ACK depend on the
			 * current state. */
			if (s->state == TCPS_FIN_WAIT_1
			 && s->snd_una == s->snd_nxt) {
				/* Our FIN got ACK'd; peer hasn't FIN'd yet. */
				if (s->retransmit_count == 0)
					tcp_update_rtt(s,
					    tcp_tick_counter - s->send_tick);
				tcp_cancel_rto(s);
				s->state = TCPS_FIN_WAIT_2;
			} else if (s->state == TCPS_LAST_ACK
			        && s->snd_una == s->snd_nxt) {
				/* Our FIN (sent in response to peer's FIN)
				 * is now ACK'd.  Connection fully closed. */
				if (s->retransmit_count == 0)
					tcp_update_rtt(s,
					    tcp_tick_counter - s->send_tick);
				tcp_cancel_rto(s);
				s->state = TCPS_CLOSED;
			}
		}

		/* In-order data delivery -- accept payload up through
		 * CLOSE_WAIT (peer can still send before we FIN), drop
		 * it after we've FIN'd. */
		unsigned data_len = len - hl;
		if (data_len > 0 && seg_seq == s->rcv_nxt
		    && (s->state == TCPS_ESTABLISHED
		     || s->state == TCPS_CLOSE_WAIT)) {
			mblk_t *data = allocb(data_len, 0);
			if (data) {
				kmemcpy(data->b_wptr,
				        body->b_rptr + hl, data_len);
				data->b_wptr += data_len;
				/* State-update before ACK send -- lo0 sync
				 * trap from T1b. */
				s->rcv_nxt += data_len;
				(void)tcp_send_segment(s, TCP_FLAG_ACK, NULL);
				reply_data_ind(q, data);
			}
		}

		/* FIN: consumes one sequence slot and triggers half-close
		 * on our side.  Only honour if it's the next byte we
		 * expect (post-data is fine). */
		if ((flags & TCP_FLAG_FIN) && seg_seq + data_len == s->rcv_nxt) {
			s->rcv_nxt += 1;
			/* state-update before send (lo0 sync) */
			enum tcp_state prev = s->state;
			if (prev == TCPS_ESTABLISHED)
				s->state = TCPS_CLOSE_WAIT;
			else if (prev == TCPS_FIN_WAIT_1
			      || prev == TCPS_FIN_WAIT_2)
				/* T1e simplification: skip TIME_WAIT and
				 * jump straight to CLOSED.  No 2*MSL wait
				 * means the same 5-tuple can be reused
				 * immediately; not a practical issue at
				 * our scale. */
				s->state = TCPS_CLOSED;
			(void)tcp_send_segment(s, TCP_FLAG_ACK, NULL);
			/* T_ORDREL_IND fires on every FIN we accept,
			 * regardless of our state.  Even if we already
			 * issued T_ORDREL_REQ ourselves (FIN_WAIT_*), the
			 * user benefits from the "peer is done sending"
			 * signal -- it means the connection is fully
			 * closed, no need to wait further. */
			(void)prev;
			reply_ordrel_ind(q);
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
	s->state    = TCPS_CLOSED;
	s->upper_rq = rq;	/* T1f retransmit kthread delivers
				 * T_DISCON_IND here on give-up */
	rq->q_ptr     = s;
	WR(rq)->q_ptr = s;
	tcp_tcb_register(s);
	return 0;
}

static int tcp_qclose(queue_t *rq)
{
	struct tcp_tcb *s = rq->q_ptr;
	if (!s) return 0;
	/* User closed an active connection without going through the
	 * graceful close (T_ORDREL_REQ) dance.  Send a RST as a
	 * failsafe so the peer sees the connection die instead of
	 * having to time out.  Skip if we're already CLOSED. */
	if (s->state == TCPS_ESTABLISHED
	 || s->state == TCPS_FIN_WAIT_1
	 || s->state == TCPS_FIN_WAIT_2
	 || s->state == TCPS_CLOSE_WAIT
	 || s->state == TCPS_LAST_ACK
	 || s->state == TCPS_SYN_SENT
	 || s->state == TCPS_SYN_RECEIVED) {
		(void)tcp_send_segment(s, TCP_FLAG_RST, NULL);
	}
	/* Listener close with queued SYNs: send RST to each peer so
	 * they don't time out waiting for SYN-ACK.  Same shape as
	 * T_DISCON_REQ-on-LISTEN above. */
	if (s->state == TCPS_LISTEN && s->pending_count > 0) {
		while (s->pending_count > 0) {
			int h = s->pending_head;
			s->remote_ip   = s->pending_q[h].remote_ip;
			s->remote_port = s->pending_q[h].remote_port;
			(void)tcp_send_segment(s, TCP_FLAG_RST, NULL);
			s->pending_head = (h + 1) % TCP_LISTEN_BACKLOG;
			s->pending_count--;
		}
	}
	if (s->bound) {
		uint32_t key = tcp_bind_key(s->local_port, 0);
		send_ip_bind(rq, IP_T_UNBIND_REQ, key);
	}
	if (s->snd_buf) freemsg(s->snd_buf);
	tcp_tcb_unregister(s);
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

/* Core accept: pop the listener's head pending SYN onto the given
 * child TCB and send SYN-ACK from it.  Caller is responsible for
 * validating that `s` is a listener with at least one queued SYN and
 * `child` is a fresh TCB in CLOSED state.  Used by both the syscall
 * path (handle_conn_res) and the in-kernel selftest. */
static int tcp_accept_into(struct tcp_tcb *s, struct tcp_tcb *child)
{
	int head = s->pending_head;
	uint32_t rip   = s->pending_q[head].remote_ip;
	uint16_t rport = s->pending_q[head].remote_port;
	uint32_t irs   = s->pending_q[head].irs;
	s->pending_head = (head + 1) % TCP_LISTEN_BACKLOG;
	s->pending_count--;

	child->local_port  = s->local_port;
	child->remote_ip   = rip;
	child->remote_port = rport;
	child->rcv_irs     = irs;
	child->rcv_nxt     = irs + 1;
	child->snd_iss     = alloc_iss();
	child->snd_una     = child->snd_iss;
	child->snd_nxt     = child->snd_iss;
	child->state       = TCPS_SYN_RECEIVED;

	return tcp_send_segment(child, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL);
}

static int handle_conn_res(queue_t *q, struct tcp_tcb *s,
                           const struct t_tcp_conn_res *req)
{
	if (s->state != TCPS_LISTEN || s->pending_count == 0) {
		reply_discon_ind(OTHERQ(q), TCP_DISCON_REASON_PROTOERR);
		return 0;
	}
	/* Resolve the user-supplied responding fd.  Must be a fresh
	 * /dev/tcp endpoint (still in CLOSED state, never bound or
	 * connected), and must not be the listener itself.  The child
	 * borrows the listener's local_port and stays un-bound at IP
	 * level -- inbound segments reach it via the listener's
	 * fan-out in tcp_input_segment. */
	struct tcp_tcb *child = tcp_tcb_from_fd(req->responding_fd);
	if (!child || child == s || child->state != TCPS_CLOSED) {
		reply_discon_ind(OTHERQ(q), TCP_DISCON_REASON_PROTOERR);
		return 0;
	}
	(void)tcp_accept_into(s, child);
	return 0;
}

static int handle_ordrel_req(queue_t *q, struct tcp_tcb *s)
{
	(void)q;
	if (s->state == TCPS_ESTABLISHED) {
		/* Graceful close, active side.  Send FIN, FIN_WAIT_1. */
		s->state = TCPS_FIN_WAIT_1;
		(void)tcp_send_segment(s, TCP_FLAG_ACK | TCP_FLAG_FIN, NULL);
		return 0;
	}
	if (s->state == TCPS_CLOSE_WAIT) {
		/* Peer already FIN'd us; respond with our FIN -> LAST_ACK. */
		s->state = TCPS_LAST_ACK;
		(void)tcp_send_segment(s, TCP_FLAG_ACK | TCP_FLAG_FIN, NULL);
		return 0;
	}
	reply_discon_ind(OTHERQ(q), TCP_DISCON_REASON_PROTOERR);
	return 0;
}

static int handle_discon_req(queue_t *q, struct tcp_tcb *s)
{
	(void)q;
	/* Abortive close: RST, then CLOSED.  Valid from any non-CLOSED
	 * post-handshake state, plus from SYN_SENT / SYN_RECEIVED /
	 * LISTEN-with-pending-incoming (the user's "reject" hook). */
	if (s->state == TCPS_CLOSED) return 0;
	if (s->state == TCPS_SYN_SENT
	 || s->state == TCPS_SYN_RECEIVED
	 || s->state == TCPS_ESTABLISHED
	 || s->state == TCPS_FIN_WAIT_1
	 || s->state == TCPS_FIN_WAIT_2
	 || s->state == TCPS_CLOSE_WAIT
	 || s->state == TCPS_LAST_ACK) {
		s->state = TCPS_CLOSED;
		(void)tcp_send_segment(s, TCP_FLAG_RST, NULL);
		return 0;
	}
	if (s->state == TCPS_LISTEN && s->pending_count > 0) {
		/* Reject every pending SYN with RST.  Stay in LISTEN.
		 * RFC 793: when the user aborts an arrived connection
		 * request, the SYN gets an RST so the peer's SYN_SENT
		 * tears down promptly instead of timing out. */
		while (s->pending_count > 0) {
			int h = s->pending_head;
			s->remote_ip   = s->pending_q[h].remote_ip;
			s->remote_port = s->pending_q[h].remote_port;
			(void)tcp_send_segment(s, TCP_FLAG_RST, NULL);
			s->pending_head = (h + 1) % TCP_LISTEN_BACKLOG;
			s->pending_count--;
		}
		s->remote_ip   = 0;
		s->remote_port = 0;
	}
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
	unsigned plen = (unsigned)msgdsize(payload);
	unsigned old  = s->snd_buf
	             ? (unsigned)(s->snd_buf->b_wptr - s->snd_buf->b_rptr) : 0;
	if (old + plen > TCP_SND_BUF_MAX) {
		freemsg(payload);
		reply_discon_ind(OTHERQ(q), TCP_DISCON_REASON_PROTOERR);
		return -1;
	}

	/* Append to snd_buf so retransmits can pull the unACK'd bytes
	 * out of it.  Single-mblk model keeps the trim logic simple
	 * (advance b_rptr on ACK). */
	mblk_t *new_buf = allocb(old + plen, 0);
	if (!new_buf) { freemsg(payload); return -1; }
	if (old) {
		kmemcpy(new_buf->b_wptr, s->snd_buf->b_rptr, old);
		new_buf->b_wptr += old;
	}
	for (mblk_t *m = payload; m; m = m->b_cont) {
		unsigned n = (unsigned)(m->b_wptr - m->b_rptr);
		if (n == 0) continue;
		kmemcpy(new_buf->b_wptr, m->b_rptr, n);
		new_buf->b_wptr += n;
	}

	if (!s->snd_buf) s->snd_buf_seq = s->snd_nxt;
	if (s->snd_buf) freemsg(s->snd_buf);
	s->snd_buf = new_buf;

	/* Send a copy of just the new bytes.  tcp_send_segment copies +
	 * frees; the canonical retransmit-source bytes live in snd_buf. */
	mblk_t *send_copy = allocb(plen, 0);
	if (send_copy) {
		kmemcpy(send_copy->b_wptr,
		        new_buf->b_rptr + old, plen);
		send_copy->b_wptr += plen;
	}
	freemsg(payload);
	return tcp_send_segment(s, TCP_FLAG_ACK | TCP_FLAG_PSH, send_copy);
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
	} else if (prim == T_TCP_CONN_RES
	    && (mp->b_wptr - mp->b_rptr)
	       >= (int)sizeof(struct t_tcp_conn_res)) {
		rc = handle_conn_res(q, s,
		                     (const struct t_tcp_conn_res *)mp->b_rptr);
		freemsg(mp);
	} else if (prim == T_TCP_ORDREL_REQ) {
		rc = handle_ordrel_req(q, s);
		freemsg(mp);
	} else if (prim == T_TCP_DISCON_REQ) {
		rc = handle_discon_req(q, s);
		freemsg(mp);
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

/* ---- Retransmit kthread (T1f) ---------------------------------- */
/*
 * Polls tcp_tick_counter (incremented at 100Hz from sched_tick).
 * For each TCB with an armed timer whose expiry has passed:
 *   - If retransmit count maxed out, abort (RST + DISCON_IND).
 *   - Else exponential-backoff RTO, re-send the pending SYN/FIN.
 */
static void tcp_retransmit_main(void *arg)
{
	(void)arg;
	uint32_t last_seen = 0;
	for (;;) {
		kthread_yield();
		uint32_t now = tcp_tick_counter;
		if (now == last_seen) continue;
		last_seen = now;

		for (int i = 0; i < TCP_MAX_TCBS; i++) {
			struct tcp_tcb *s = tcp_tcb_table[i];
			if (!s || s->rto_expires == 0)         continue;
			if ((int32_t)(now - s->rto_expires) < 0) continue;

			if (s->retransmit_count >= TCP_MAX_RETRANSMITS) {
				/* Give up.  Abort with RST + tell user. */
				uint8_t f = s->pending_flags;
				(void)f;
				s->state       = TCPS_CLOSED;
				s->rto_expires = 0;
				if (s->upper_rq)
					reply_discon_ind(s->upper_rq,
					    TCP_DISCON_REASON_TIMEOUT);
				continue;
			}

			/* Back off, re-send, re-arm.  Don't sample RTT on
			 * the eventual ACK (Karn): retransmit_count > 0
			 * suppresses sampling in all ACK paths. */
			uint8_t saved_count = s->retransmit_count + 1;
			s->rto_ticks <<= 1;
			if (s->rto_ticks > TCP_RTO_MAX) s->rto_ticks = TCP_RTO_MAX;

			/* Data retransmit takes priority: if snd_buf has
			 * unACK'd bytes, re-send them at snd_buf_seq.
			 * tcp_emit_segment doesn't touch snd_nxt, so the
			 * regular bookkeeping is unchanged. */
			if (s->snd_buf
			    && s->snd_buf->b_wptr > s->snd_buf->b_rptr) {
				unsigned plen = (unsigned)
				    (s->snd_buf->b_wptr - s->snd_buf->b_rptr);
				(void)tcp_emit_segment(s, s->snd_buf_seq,
				    TCP_FLAG_ACK | TCP_FLAG_PSH,
				    s->snd_buf->b_rptr, plen);
			} else if (s->pending_flags
			    & (TCP_FLAG_SYN | TCP_FLAG_FIN)) {
				/* SYN/FIN retransmit -- the segment
				 * already consumed a slot of seq space
				 * when first sent, so re-emit at the same
				 * seq.  Use snd_una which is the oldest
				 * unACK'd byte (== the SYN/FIN seq). */
				(void)tcp_emit_segment(s, s->snd_una,
				    s->pending_flags, NULL, 0);
			}

			s->retransmit_count = saved_count;
			s->rto_expires      = now + s->rto_ticks;
		}
	}
}

void tcp_module_init(void)
{
	streams_register("tcp", &tcp_streamtab);
	cdev_register(CDEV_MAJ_TCP, "tcp", &ip_streamtab);
}

/* /proc/tcp walker.  cb returns the per-connection row info to the
 * caller; we don't lock the table -- connections coming and going
 * during a /proc read is OK, snapshot semantics are loose. */
static const char *tcp_state_names[] = {
	"CLOSED", "BOUND", "LISTEN", "SYN_SENT", "SYN_RECEIVED",
	"ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2", "CLOSE_WAIT", "LAST_ACK",
};

void tcp_for_each_tcb(void (*cb)(const struct tcp_tcb_view *v, void *arg),
                      void *arg)
{
	for (int i = 0; i < TCP_MAX_TCBS; i++) {
		struct tcp_tcb *s = tcp_tcb_table[i];
		if (!s) continue;
		struct tcp_tcb_view v = {
			.state_name  = (s->state < (int)(sizeof(tcp_state_names)
			                / sizeof(tcp_state_names[0])))
			               ? tcp_state_names[s->state]
			               : "?",
			.local_port  = s->local_port,
			.remote_ip   = s->remote_ip,
			.remote_port = s->remote_port,
			.snd_buf_len = s->snd_buf
			    ? (uint32_t)(s->snd_buf->b_wptr - s->snd_buf->b_rptr)
			    : 0,
			.rto_ms      = s->rto_ticks * 10,
			.srtt_ms     = s->srtt_ticks * 10,
			.backlog_depth = s->pending_count,
		};
		cb(&v, arg);
	}
}

/* Late init: spawn the retransmit kthread.  Called from main.c
 * AFTER sched_init (kthread_create derefs curthread under the
 * hood; tcp_module_init runs from streams_head_init which is
 * pre-sched). */
void tcp_init_late(void)
{
	kthread_create("tcp_rtx", tcp_retransmit_main, NULL);
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
	struct stdata *L = NULL, *C = NULL, *R = NULL;
	int rc = -1;

	L = stream_build_kernel(&ip_streamtab, "tcp_listen", 0);
	C = stream_build_kernel(&ip_streamtab, "tcp_client", 0);
	R = stream_build_kernel(&ip_streamtab, "tcp_respond", 0);
	if (!L || !C || !R) goto out;
	if (stream_push_kernel(L, "tcp") < 0) goto out;
	if (stream_push_kernel(C, "tcp") < 0) goto out;
	if (stream_push_kernel(R, "tcp") < 0) goto out;

	/* Bind L (listener) and C (client).  R stays unbound -- accept
	 * grafts the connection state onto it without going through IP
	 * bind, since R receives inbound segments via the listener's
	 * fan-out. */
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

	/* Accept: graft the pending SYN onto R's TCB.  The syscall path
	 * (handle_conn_res) does this by resolving an fd; from kernel
	 * context we hop straight to the helper since L's and R's TCBs
	 * sit behind their respective sd_wq->q_ptr. */
	{
		struct tcp_tcb *L_tcb = L->sd_wq->q_next->q_ptr;
		struct tcp_tcb *R_tcb = R->sd_wq->q_next->q_ptr;
		if (!L_tcb || !R_tcb) goto out;
		if (tcp_accept_into(L_tcb, R_tcb) < 0) goto out;
	}

	/* T_CONN_CON now fires on C (active side) and R (responder).
	 * L stays in LISTEN -- this is the key T1d.5 invariant. */
	if (drain_for_prim(C, T_TCP_CONN_CON) < 0) goto out;
	if (drain_for_prim(R, T_TCP_CONN_CON) < 0) goto out;

	/* T1c: bidirectional data exchange.  Data flows on the accepted
	 * connection (R), not the listener. */
	static const char msg_c2r[] = "hello-from-client";
	static const char msg_r2c[] = "and-from-responder";

	if (send_data_req(C, msg_c2r, sizeof(msg_c2r) - 1) < 0) goto out;
	if (drain_data_ind(R, msg_c2r, sizeof(msg_c2r) - 1) < 0) goto out;

	if (send_data_req(R, msg_r2c, sizeof(msg_r2c) - 1) < 0) goto out;
	if (drain_data_ind(C, msg_r2c, sizeof(msg_r2c) - 1) < 0) goto out;

	/* T1e graceful close on the accepted connection. */
	{
		mblk_t *mp = allocb(sizeof(struct t_tcp_ordrel_req), 0);
		if (!mp) goto out;
		mp->b_datap->db_type = M_PROTO;
		struct t_tcp_ordrel_req *r =
		    (struct t_tcp_ordrel_req *)mp->b_wptr;
		r->prim    = T_TCP_ORDREL_REQ;
		r->_pad[0] = r->_pad[1] = r->_pad[2] = 0;
		mp->b_wptr += sizeof(*r);
		putnext(C->sd_wq, mp);
	}
	if (drain_for_prim(R, T_TCP_ORDREL_IND) < 0) goto out;
	{
		mblk_t *mp = allocb(sizeof(struct t_tcp_ordrel_req), 0);
		if (!mp) goto out;
		mp->b_datap->db_type = M_PROTO;
		struct t_tcp_ordrel_req *r =
		    (struct t_tcp_ordrel_req *)mp->b_wptr;
		r->prim    = T_TCP_ORDREL_REQ;
		r->_pad[0] = r->_pad[1] = r->_pad[2] = 0;
		mp->b_wptr += sizeof(*r);
		putnext(R->sd_wq, mp);
	}
	if (drain_for_prim(C, T_TCP_ORDREL_IND) < 0) goto out;

	rc = 0;

out:
	if (L) stream_destroy_kernel(L);
	if (C) stream_destroy_kernel(C);
	if (R) stream_destroy_kernel(R);
	return rc;
}

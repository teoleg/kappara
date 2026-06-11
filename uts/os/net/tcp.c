/*
 * uts/os/net/tcp.c -- TCP as a STREAMS module above IP.
 *
 * Phase T1a: skeleton.  The module is registered, /dev/tcp's cdev
 * autopushes "tcp" on top of an ip_streamtab stream (same trick as
 * /dev/icmp and /dev/udp), and T_BIND_REQ / T_UNBIND_REQ are wired
 * through to IP's per-(proto, key) binding table.  The bind key
 * packs (local_port << 16) | (remote_port=0 for now), leaving room
 * for T1b's exact-match demux of established connections.
 *
 * All other TPI primitives (T_CONN_REQ, T_DATA_REQ, T_ORDREL_REQ,
 * T_DISCON_REQ) are recognised at this phase but bounce back with
 * T_DISCON_IND{reason=NOTSUP}.  Subsequent phases implement them
 * without changing the user-visible primitive numbering -- the
 * header (include/kappara/net/tcp.h) is the locked-in ABI.
 *
 * Inbound segments demuxed by IP land in tcp_rq_putp wrapped as
 * M_PROTO{IP_T_UNITDATA_IND} + M_DATA(tcp segment).  T1a logs and
 * drops them; T1b grows the state machine that processes them.
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
	TCPS_BOUND         = 1,	/* local addr/port assigned, no connect yet */
	TCPS_LISTEN        = 2,	/* T1d */
	TCPS_SYN_SENT      = 3,	/* T1b */
	TCPS_SYN_RECEIVED  = 4,	/* T1d */
	TCPS_ESTABLISHED   = 5,
	TCPS_FIN_WAIT_1    = 6,	/* T1e */
	TCPS_FIN_WAIT_2    = 7,
	TCPS_CLOSE_WAIT    = 8,
	TCPS_LAST_ACK      = 9,
};

struct tcp_tcb {
	enum tcp_state  state;

	/* Local endpoint (set at T_BIND_REQ). */
	uint16_t   local_port;
	int        bound;	/* IP_T_BIND_ACK received */

	/* Remote endpoint (set at T_CONN_REQ / T_CONN_IND in T1b/T1d). */
	uint32_t   remote_ip;
	uint16_t   remote_port;

	/* Sequence-number bookkeeping (RFC 793 section 3.2).  Used by
	 * T1b onward. */
	uint32_t   snd_una;	/* oldest unacknowledged seq */
	uint32_t   snd_nxt;	/* next seq to send          */
	uint32_t   snd_iss;	/* initial seq we picked     */
	uint32_t   rcv_nxt;	/* next seq we expect to recv */
	uint32_t   rcv_irs;	/* initial seq peer picked   */
};

/* TCP_BIND_KEY: pack (local_port, remote_port) into the 32-bit IP
 * binding key.  For T1a the remote port is always 0 (any).  T1b/T1d
 * will use exact-match keys for established connections so IP demux
 * delivers a segment straight to the right stream.  */
static uint32_t tcp_bind_key(uint16_t lport, uint16_t rport)
{
	return ((uint32_t)lport << 16) | (uint32_t)rport;
}

/* Ephemeral port allocator (same shape UDP uses).  Skip well-known
 * ports below 1024. */
static uint16_t tcp_ephemeral_next = 49152;

static uint16_t alloc_ephemeral_port(void)
{
	uint16_t p = tcp_ephemeral_next++;
	if (tcp_ephemeral_next == 0) tcp_ephemeral_next = 49152;
	return p;
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
		uint32_t key = tcp_bind_key(s->local_port, s->remote_port);
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
	send_ip_bind(OTHERQ(q), IP_T_BIND_REQ,
	             tcp_bind_key(port, 0));
	return 0;
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
	} else if (prim == T_TCP_CONN_REQ
	        || prim == T_TCP_CONN_RES
	        || prim == T_TCP_DATA_REQ
	        || prim == T_TCP_ORDREL_REQ
	        || prim == T_TCP_DISCON_REQ) {
		/* Recognised primitives not yet implemented at this
		 * phase.  Bounce a T_DISCON_IND so the user knows it
		 * isn't going to happen. */
		reply_discon_ind(OTHERQ(q), TCP_DISCON_REASON_NOTSUP);
		rc = 0;
	}
	freemsg(mp);
	return rc;
}

/* ---- Read side: IP bind acks + (later) inbound segments ---------- */

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
	/* IP_T_UNBIND_ACK: silent. */
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

	if (prim == IP_T_UNITDATA_IND) {
		/* Inbound segment.  T1a just drops it; T1b grows the
		 * state machine. */
		freemsg(mp);
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
 * T1a: build a tcp stream in-kernel, send T_BIND_REQ port=54321,
 * expect T_BIND_ACK with the same port.  Subsequent phases add
 * connect / data / close to the test in step with their handler
 * implementations.
 */

int tcp_selftest_run(void)
{
	struct stdata *sd = stream_build_kernel(&ip_streamtab,
	                                        "tcp_selftest", 0);
	if (!sd) return -1;
	if (stream_push_kernel(sd, "tcp") < 0) {
		stream_destroy_kernel(sd);
		return -1;
	}
	int rc = -1;

	{
		mblk_t *mp = allocb(sizeof(struct t_tcp_bind_req), 0);
		if (!mp) goto out;
		mp->b_datap->db_type = M_PROTO;
		struct t_tcp_bind_req *r = (struct t_tcp_bind_req *)mp->b_wptr;
		r->prim    = T_TCP_BIND_REQ;
		r->_pad[0] = 0;
		r->port    = 54321;
		mp->b_wptr += sizeof(*r);
		putnext(sd->sd_wq, mp);
	}

	mblk_t *got = NULL;
	for (int i = 0; i < 32 && !got; i++) {
		got = getq(sd->sd_rq);
		if (!got) kthread_yield();
	}
	if (!got) goto out;
	if (got->b_datap->db_type != M_PROTO
	    || got->b_rptr[0] != T_TCP_BIND_ACK) {
		freemsg(got);
		goto out;
	}
	const struct t_tcp_bind_ack *a =
	    (const struct t_tcp_bind_ack *)got->b_rptr;
	if (a->port == 54321) rc = 0;
	freemsg(got);

out:
	stream_destroy_kernel(sd);
	return rc;
}

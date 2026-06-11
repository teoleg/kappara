/*
 * uts/os/net/pktfilter.c -- demo pushable STREAMS filter module.
 *
 * See include/kappara/net/pktfilter.h for the full layering picture.
 * In short: I_PUSH "pktfilter" inserts this module between the
 * stream head and whatever's below; wput inspects M_PROTO traffic
 * heading down, drops T_UNITDATA_REQ packets that match a runtime-
 * configurable rule, and counts pass/drop for stats queries.
 *
 * The module is intentionally tiny because that's the point: STREAMS
 * modules are supposed to be small, composable, and runtime-attached.
 * No autopush, no per-protocol special-casing -- it's a pure shape-
 * matching filter on TPI primitives.
 */

#include <stdint.h>

#include "kappara/core/kmem.h"
#include "kappara/net/pktfilter.h"
#include "kappara/net/udp.h"		/* T_UNITDATA_REQ + struct t_unitdata_req */
#include "kappara/core/printk.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/core/string.h"

struct pf_state {
	int        enabled;
	uint32_t   drop_dst_ip;
	uint16_t   drop_dst_port;
	uint32_t   passed;
	uint32_t   dropped;
};

/* ---- qopen / qclose ---------------------------------------------- */

static int pf_qopen(queue_t *rq)
{
	struct pf_state *s = kmalloc(sizeof(*s));
	if (!s) return -1;
	kmemset(s, 0, sizeof(*s));
	rq->q_ptr     = s;
	WR(rq)->q_ptr = s;
	return 0;
}

static int pf_qclose(queue_t *rq)
{
	struct pf_state *s = rq->q_ptr;
	if (!s) return 0;
	rq->q_ptr     = NULL;
	WR(rq)->q_ptr = NULL;
	kfree(s);
	return 0;
}

/* ---- Replies up the stream head ---------------------------------- */

static void pf_send_set_ack(queue_t *q)
{
	mblk_t *mp = allocb(sizeof(struct pf_set_ack), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct pf_set_ack *a = (struct pf_set_ack *)mp->b_wptr;
	a->prim    = PF_T_SET_ACK;
	a->_pad[0] = a->_pad[1] = a->_pad[2] = 0;
	mp->b_wptr += sizeof(*a);
	putnext(OTHERQ(q), mp);
}

static void pf_send_stats(queue_t *q, const struct pf_state *s)
{
	mblk_t *mp = allocb(sizeof(struct pf_stats_res), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct pf_stats_res *r = (struct pf_stats_res *)mp->b_wptr;
	r->prim    = PF_T_STATS_RES;
	r->_pad[0] = r->_pad[1] = r->_pad[2] = 0;
	r->passed  = s->passed;
	r->dropped = s->dropped;
	mp->b_wptr += sizeof(*r);
	putnext(OTHERQ(q), mp);
}

/* ---- Drop decision ----------------------------------------------- */

static int pf_should_drop(const struct pf_state *s,
                          const struct t_unitdata_req *req)
{
	if (!s->enabled) return 0;
	if (s->drop_dst_ip   != 0 && req->dst_ip   != s->drop_dst_ip)
		return 0;
	if (s->drop_dst_port != 0 && req->dst_port != s->drop_dst_port)
		return 0;
	return 1;
}

/* ---- Write side (downstream) ------------------------------------- */

static int pf_wq_putp(queue_t *q, mblk_t *mp)
{
	struct pf_state *s = q->q_ptr;
	if (!s || !mp) {
		freemsg(mp);
		return -1;
	}

	/* Only M_PROTO mblks carry primitives we recognise.  Pass
	 * everything else (M_DATA, M_IOCTL, M_FLUSH, ...) straight
	 * through so we don't break unrelated control paths. */
	if (mp->b_datap->db_type != M_PROTO
	    || (mp->b_wptr - mp->b_rptr) < 1)
		return putnext(q, mp);

	uint8_t prim = mp->b_rptr[0];

	/* Our own control primitives are consumed here and ACK back up. */
	if (prim == PF_T_SET_REQ
	    && (mp->b_wptr - mp->b_rptr) >= (int)sizeof(struct pf_set_req)) {
		const struct pf_set_req *req =
		    (const struct pf_set_req *)mp->b_rptr;
		s->enabled       = req->enabled ? 1 : 0;
		s->drop_dst_ip   = req->drop_dst_ip;
		s->drop_dst_port = req->drop_dst_port;
		freemsg(mp);
		pf_send_set_ack(q);
		return 0;
	}
	if (prim == PF_T_STATS_REQ) {
		freemsg(mp);
		pf_send_stats(q, s);
		return 0;
	}

	/* T_UNITDATA_REQ is the only primitive we filter on; the rest
	 * (T_BIND_REQ, etc) pass through so UDP keeps working. */
	if (prim == T_UNITDATA_REQ
	    && (mp->b_wptr - mp->b_rptr)
	       >= (int)sizeof(struct t_unitdata_req)) {
		const struct t_unitdata_req *req =
		    (const struct t_unitdata_req *)mp->b_rptr;
		if (pf_should_drop(s, req)) {
			s->dropped++;
			kprintf("pktfilter: dropped UDP -> %u.%u.%u.%u:%u\n",
			        (req->dst_ip >> 24) & 0xff,
			        (req->dst_ip >> 16) & 0xff,
			        (req->dst_ip >>  8) & 0xff,
			         req->dst_ip        & 0xff,
			        (unsigned)req->dst_port);
			freemsg(mp);
			return 0;
		}
		s->passed++;
	}

	return putnext(q, mp);
}

/* ---- Read side (upstream) ---------------------------------------- */

static int pf_rq_putp(queue_t *q, mblk_t *mp)
{
	/* Passthrough.  pktfilter pushed above udp doesn't see inbound
	 * UDP packets anyway (IP demux uses put() straight into udp's
	 * rq), but we still pass control traffic (T_BIND_ACK from udp,
	 * etc) so the handshakes work. */
	return putnext(q, mp);
}

/* ---- Module wiring ----------------------------------------------- */

static struct module_info pf_minfo = {
	.mi_idnum  = 800,
	.mi_idname = "pktfilter",
	.mi_minpsz = 0,
	.mi_maxpsz = 65535,
	.mi_hiwat  = 8192,
	.mi_lowat  = 4096,
};

static struct qinit pf_rinit = {
	.qi_putp  = pf_rq_putp,
	.qi_qopen = pf_qopen,
	.qi_qclose= pf_qclose,
	.qi_minfo = &pf_minfo,
};
static struct qinit pf_winit = {
	.qi_putp  = pf_wq_putp,
	.qi_minfo = &pf_minfo,
};
struct streamtab pktfilter_streamtab = {
	.st_rdinit = &pf_rinit,
	.st_wrinit = &pf_winit,
};

void pktfilter_module_init(void)
{
	streams_register("pktfilter", &pktfilter_streamtab);
}

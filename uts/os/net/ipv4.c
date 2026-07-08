/*
 * uts/os/net/ipv4.c -- IPv4 as a STREAMS multiplexor driver
 *
 * IP is a full SVR4-style multiplexor: multiple lower streams (one
 * per netif, joined via I_LINK from ip_init) AND multiple upper
 * bindings (one per transport module instance, registered via
 * M_PROTO{IP_T_BIND_REQ}).  All routing is putnext + put across
 * queue boundaries -- there are no direct C calls between IP and
 * a transport module.
 *
 * Lower side
 * ----------
 * `ip_lowers[]` keeps (muxid, qbot, nif) per linked netif.  ip_wput
 * on M_IOCTL{I_LINK} populates qbot+muxid; the boot loop in ip_init
 * backfills nif via ip_lower_register so route lookup can match a
 * destination against the lower's (ip, netmask).
 *
 * Upper side
 * ----------
 * `ip_uppers[]` keeps (proto, key, upper_rq) per bound module
 * instance.  ip_wput on M_PROTO{IP_T_BIND_REQ} records the
 * sender's OTHERQ (its read queue) so ip_rput can deliver
 * demultiplexed packets back across the stream boundary using
 * put() (not putnext -- there is no shared q_next chain across
 * streams).  Multiple uppers can bind the same proto; rx is
 * multicast via dupmsg so each gets a copy.  Filtering by deeper
 * key (ICMP id, UDP local port) is the module's job.
 *
 * Send side
 * ---------
 * M_PROTO{IP_T_SEND_REQ} + M_DATA(payload).  ip_wput prepends the
 * IPv4 header in the payload's reserved headroom, routes by
 * longest-prefix-match against ip_lowers[], putnexts down into
 * lower->qbot.
 *
 * Receive side
 * ------------
 * Each lower's drv_rq->q_next was rewired by I_LINK to point at
 * IP's drv_rq.  lo0_wput's putnext(OTHERQ) hits IP's rput; future
 * SLIP/Ethernet drivers' rx threads will do the same.  ip_rput
 * validates the header, strips it, demuxes via ip_uppers[].
 */

#include <stdint.h>

#include "kappara/core/kmem.h"
#include "kappara/net/ip.h"
#include "kappara/net/netif.h"
#include "kappara/core/printk.h"
#include "kappara/proc/sched.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/core/string.h"

static uint16_t ip_id_next;

uint16_t ip_checksum(const void *buf, unsigned len)
{
	const uint8_t *p = (const uint8_t *)buf;
	uint32_t sum = 0;
	while (len >= 2) {
		uint16_t w = (uint16_t)((p[0] << 8) | p[1]);
		sum += w;
		p   += 2;
		len -= 2;
	}
	if (len) sum += (uint32_t)(p[0] << 8);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

/* ---- Mux state ----------------------------------------------------- */

#define IP_MAX_LOWERS	4
#define IP_MAX_UPPERS	16

static struct ip_lower {
	int           muxid;
	queue_t      *qbot;
	struct netif *nif;
} ip_lowers[IP_MAX_LOWERS];

static struct ip_upper {
	int       active;
	uint8_t   proto;
	uint32_t  key;		/* opaque, module-defined */
	queue_t  *upper_rq;	/* IP rput -> put(upper_rq, mp) */
} ip_uppers[IP_MAX_UPPERS];

static struct stdata *ip_ctl_sd;

/* ---- Routing ------------------------------------------------------- */

static struct ip_lower *ip_route(uint32_t dst_ip)
{
	struct ip_lower *best = NULL;
	unsigned best_prefix = 0;
	for (int i = 0; i < IP_MAX_LOWERS; i++) {
		struct ip_lower *l = &ip_lowers[i];
		if (!l->qbot || !l->nif) continue;
		if ((dst_ip & l->nif->netmask)
		    != (l->nif->ip & l->nif->netmask))
			continue;
		uint32_t v = l->nif->netmask;
		unsigned p = 0;
		while (v) { v &= v - 1; p++; }
		if (!best || p >= best_prefix) {
			best = l;
			best_prefix = p;
		}
	}
	return best;
}

void ip_lower_register(int muxid, struct netif *nif)
{
	for (int i = 0; i < IP_MAX_LOWERS; i++) {
		if (ip_lowers[i].muxid == muxid && ip_lowers[i].qbot) {
			ip_lowers[i].nif = nif;
			return;
		}
	}
	kprintf("ip: lower_register muxid=%d not found\n", muxid);
}

uint32_t ip_route_src(uint32_t dst_ip)
{
	struct ip_lower *l = ip_route(dst_ip);
	return (l && l->nif) ? l->nif->ip : 0;
}

long ip_attach_stream(struct stdata *netif_sd, struct netif *nif)
{
	if (!ip_ctl_sd || !netif_sd || !nif) return -1;
	long muxid = stream_ilink(ip_ctl_sd, netif_sd);
	if (muxid <= 0) return -1;
	ip_lower_register((int)muxid, nif);
	return muxid;
}

/* ---- Bind table management ---------------------------------------- */

static int ip_bind(uint8_t proto, uint32_t key, queue_t *upper_rq)
{
	/* Reject exact duplicates: same (proto, key, upper_rq) bound
	 * twice indicates a leak.  Same (proto, key) from a DIFFERENT
	 * upper is fine -- that's the multicast case (multiple ping
	 * processes, multiple raw socket listeners). */
	for (int i = 0; i < IP_MAX_UPPERS; i++) {
		if (!ip_uppers[i].active) continue;
		if (ip_uppers[i].proto    == proto
		 && ip_uppers[i].key      == key
		 && ip_uppers[i].upper_rq == upper_rq)
			return -1;
	}
	for (int i = 0; i < IP_MAX_UPPERS; i++) {
		if (!ip_uppers[i].active) {
			ip_uppers[i].active   = 1;
			ip_uppers[i].proto    = proto;
			ip_uppers[i].key      = key;
			ip_uppers[i].upper_rq = upper_rq;
			return 0;
		}
	}
	return -1;
}

static int ip_unbind(uint8_t proto, uint32_t key, queue_t *upper_rq)
{
	for (int i = 0; i < IP_MAX_UPPERS; i++) {
		if (!ip_uppers[i].active) continue;
		if (ip_uppers[i].proto    != proto)    continue;
		if (ip_uppers[i].key      != key)      continue;
		if (ip_uppers[i].upper_rq != upper_rq) continue;
		ip_uppers[i].active   = 0;
		ip_uppers[i].upper_rq = NULL;
		return 0;
	}
	return -1;
}

/* Reply M_PROTO with prim flipped to the ACK / NAK opcode, sent
 * back UP this stream's read side via putnext.  The bind came down
 * from one queue above us; the reply rides the same chain back up
 * and reaches that queue's rput. */
static void ip_bind_reply(queue_t *ip_drv_wq, uint8_t prim, uint8_t proto,
                          uint32_t key)
{
	mblk_t *mp = allocb(sizeof(struct ip_bind_meta), 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	struct ip_bind_meta *m = (struct ip_bind_meta *)mp->b_wptr;
	m->prim   = prim;
	m->proto  = proto;
	m->key    = key;
	m->_pad[0] = m->_pad[1] = 0;
	mp->b_wptr += sizeof(*m);
	putnext(OTHERQ(ip_drv_wq), mp);
}

/* ---- IP wput -- send side + bind handling ------------------------- */

static int ip_handle_link(queue_t *q, mblk_t *mp)
{
	struct iocblk *ic = (struct iocblk *)mp->b_rptr;
	struct linkblk *lk = mp->b_cont
	    ? (struct linkblk *)mp->b_cont->b_rptr : NULL;
	int ok = 0;

	if (lk && ic->ic_cmd == I_LINK) {
		for (int i = 0; i < IP_MAX_LOWERS; i++) {
			if (ip_lowers[i].muxid == 0) {
				ip_lowers[i].muxid = lk->l_index;
				ip_lowers[i].qbot  = lk->l_qbot;
				ip_lowers[i].nif   = NULL;
				ok = 1;
				break;
			}
		}
	} else if (lk && ic->ic_cmd == I_UNLINK) {
		for (int i = 0; i < IP_MAX_LOWERS; i++) {
			if (ip_lowers[i].muxid == lk->l_index) {
				ip_lowers[i].muxid = 0;
				ip_lowers[i].qbot  = NULL;
				ip_lowers[i].nif   = NULL;
				ok = 1;
				break;
			}
		}
	}

	ic->ic_error = ok ? 0 : -1;
	mp->b_datap->db_type = ok ? M_IOCACK : M_IOCNAK;
	putnext(OTHERQ(q), mp);
	return 0;
}

static int ip_handle_send(mblk_t *meta_mp)
{
	struct ip_send_meta *meta = (struct ip_send_meta *)meta_mp->b_rptr;
	mblk_t *mp = meta_mp->b_cont;
	meta_mp->b_cont = NULL;
	if (!mp) {
		freemsg(meta_mp);
		return -1;
	}

	struct ip_lower *lower = ip_route(meta->dst_ip);
	if (!lower) {
		kprintf("ip: no route to %u.%u.%u.%u\n",
			(meta->dst_ip >> 24) & 0xff,
			(meta->dst_ip >> 16) & 0xff,
			(meta->dst_ip >>  8) & 0xff,
			 meta->dst_ip        & 0xff);
		freemsg(meta_mp);
		freemsg(mp);
		return -1;
	}

	unsigned payload = (unsigned)(mp->b_wptr - mp->b_rptr);
	if (payload > lower->nif->mtu - IP_HDR_LEN) {
		kprintf("ip: payload %u > MTU %u (no frag)\n",
			payload, lower->nif->mtu - IP_HDR_LEN);
		freemsg(meta_mp);
		freemsg(mp);
		return -1;
	}

	if ((size_t)(mp->b_rptr - mp->b_datap->db_base) < IP_HDR_LEN) {
		kprintf("ip: no IP headroom\n");
		freemsg(meta_mp);
		freemsg(mp);
		return -1;
	}
	mp->b_rptr -= IP_HDR_LEN;

	struct ip_hdr *h = (struct ip_hdr *)mp->b_rptr;
	h->ver_ihl   = 0x45;
	h->tos       = 0;
	h->total_len = htons16((uint16_t)(IP_HDR_LEN + payload));
	h->id        = htons16(++ip_id_next);
	h->frag_off  = 0;
	h->ttl       = IP_DEFAULT_TTL;
	h->proto     = meta->proto;
	h->checksum  = 0;
	h->src_ip    = htonl32(lower->nif->ip);
	h->dst_ip    = htonl32(meta->dst_ip);
	uint16_t cs  = ip_checksum(h, IP_HDR_LEN);
	h->checksum  = htons16(cs);

	freemsg(meta_mp);
	return putnext(lower->qbot, mp);
}

static int ip_handle_bind(queue_t *q, mblk_t *mp)
{
	if ((mp->b_wptr - mp->b_rptr) < (int)sizeof(struct ip_bind_meta)) {
		freemsg(mp);
		return -1;
	}
	struct ip_bind_meta *m = (struct ip_bind_meta *)mp->b_rptr;
	uint8_t  prim  = m->prim;
	uint8_t  proto = m->proto;
	uint32_t key   = m->key;
	freemsg(mp);

	/* The bind came down from the module directly above us on this
	 * stream.  Walking q (IP's drv_wq) → OTHERQ → q_next gives us
	 * the upper module's READ queue, which is where IP rput will
	 * later put() demuxed packets.  putnext sends the ACK back up
	 * the same chain. */
	queue_t *upper_rq = OTHERQ(q)->q_next;
	if (!upper_rq) {
		ip_bind_reply(q, IP_T_BIND_NAK, proto, key);
		return -1;
	}
	int rc = (prim == IP_T_BIND_REQ)
		? ip_bind(proto, key, upper_rq)
		: ip_unbind(proto, key, upper_rq);

	uint8_t reply_prim;
	if (prim == IP_T_BIND_REQ)
		reply_prim = (rc == 0) ? IP_T_BIND_ACK : IP_T_BIND_NAK;
	else
		reply_prim = IP_T_UNBIND_ACK;	/* unbind never naks */
	ip_bind_reply(q, reply_prim, proto, key);
	return 0;
}

static int ip_wput(queue_t *q, mblk_t *mp)
{
	if (!mp) return 0;

	if (mp->b_datap->db_type == M_IOCTL) {
		struct iocblk *ic = (struct iocblk *)mp->b_rptr;
		if (ic->ic_cmd == I_LINK || ic->ic_cmd == I_UNLINK)
			return ip_handle_link(q, mp);
		ic->ic_error = -1;
		mp->b_datap->db_type = M_IOCNAK;
		putnext(OTHERQ(q), mp);
		return 0;
	}

	if (mp->b_datap->db_type == M_PROTO) {
		if ((mp->b_wptr - mp->b_rptr) < 1) {
			freemsg(mp);
			return -1;
		}
		uint8_t prim = mp->b_rptr[0];
		if (prim == IP_T_SEND_REQ) {
			if ((mp->b_wptr - mp->b_rptr)
			    < (int)sizeof(struct ip_send_meta)) {
				freemsg(mp);
				return -1;
			}
			return ip_handle_send(mp);
		}
		if (prim == IP_T_BIND_REQ || prim == IP_T_UNBIND_REQ)
			return ip_handle_bind(q, mp);
		freemsg(mp);
		return -1;
	}

	freemsg(mp);
	return 0;
}

/* ---- IP rput -- demux ---------------------------------------------- */
/*
 * After stripping the IP header we prepend an M_PROTO
 * IP_T_UNITDATA_IND carrying (src_ip, dst_ip) so each upper module
 * sees the source address without having to keep the IP header
 * around.  Modules read the indication, advance to b_cont for the
 * actual payload, and free the whole chain when done.
 */

static mblk_t *build_unitdata_ind(uint8_t proto,
                                  uint32_t src_ip, uint32_t dst_ip)
{
	mblk_t *ind = allocb(sizeof(struct ip_unitdata_ind), 0);
	if (!ind) return NULL;
	ind->b_datap->db_type = M_PROTO;
	struct ip_unitdata_ind *u = (struct ip_unitdata_ind *)ind->b_wptr;
	u->prim    = IP_T_UNITDATA_IND;
	u->proto   = proto;
	u->_pad[0] = u->_pad[1] = 0;
	u->src_ip  = src_ip;
	u->dst_ip  = dst_ip;
	ind->b_wptr += sizeof(*u);
	return ind;
}

static void ip_demux(uint8_t proto, uint32_t src_ip, uint32_t dst_ip,
                     mblk_t *body)
{
	queue_t *matches[IP_MAX_UPPERS];
	int n = 0;
	for (int i = 0; i < IP_MAX_UPPERS && n < IP_MAX_UPPERS; i++) {
		if (!ip_uppers[i].active)         continue;
		if (ip_uppers[i].proto != proto)  continue;
		matches[n++] = ip_uppers[i].upper_rq;
	}
	if (n == 0) {
		freemsg(body);
		return;
	}
	/* Multicast: each match gets its own (ind, body_copy) chain.
	 * dupmsg shares the underlying dblks via db_ref++ so only the
	 * tiny mblk_t headers are duplicated. */
	for (int j = 0; j < n - 1; j++) {
		mblk_t *body_cpy = dupmsg(body);
		mblk_t *ind = build_unitdata_ind(proto, src_ip, dst_ip);
		if (!ind || !body_cpy) {
			if (body_cpy) freemsg(body_cpy);
			if (ind)      freemsg(ind);
			continue;
		}
		ind->b_cont = body_cpy;
		put(matches[j], ind);
	}
	mblk_t *ind = build_unitdata_ind(proto, src_ip, dst_ip);
	if (!ind) {
		freemsg(body);
		return;
	}
	ind->b_cont = body;
	put(matches[n - 1], ind);
}

static int ip_rput(queue_t *q, mblk_t *mp)
{
	(void)q;
	if (!mp) return 0;
	if (mp->b_datap->db_type != M_DATA) {
		freemsg(mp);
		return 0;
	}
	unsigned len = (unsigned)(mp->b_wptr - mp->b_rptr);
	if (len < IP_HDR_LEN) {
		kprintf("ip_rput: runt %u\n", len);
		freemsg(mp);
		return 0;
	}

	struct ip_hdr *h = (struct ip_hdr *)mp->b_rptr;
	if ((h->ver_ihl >> 4) != 4) {
		kprintf("ip_rput: not IPv4 (ver_ihl=0x%x)\n", h->ver_ihl);
		freemsg(mp);
		return 0;
	}
	unsigned ihl_bytes = (unsigned)((h->ver_ihl & 0xf) * 4);
	if (ihl_bytes < IP_HDR_LEN || ihl_bytes > len) {
		kprintf("ip_rput: bad IHL %u\n", ihl_bytes);
		freemsg(mp);
		return 0;
	}
	uint16_t total = ntohs16(h->total_len);
	if (total > len) {
		kprintf("ip_rput: total_len %u > buf %u\n", total, len);
		freemsg(mp);
		return 0;
	}
	if (total < IP_HDR_LEN) {
		kprintf("ip_rput: total_len %u < header\n", total);
		freemsg(mp);
		return 0;
	}
	/* Trim Ethernet trailing pad.  Real NICs pad short frames to the
	 * 60-byte wire minimum (QEMU's slirp doesn't, so only real
	 * networks hit this).  L4 checksums cover exactly total_len
	 * bytes -- leaving the pad on makes every small TCP segment
	 * (bare ACKs especially) fail software verification. */
	if (total < len) {
		mp->b_wptr = mp->b_rptr + total;
		if (mp->b_cont) {
			freemsg(mp->b_cont);
			mp->b_cont = NULL;
		}
	}
	if (ip_checksum(h, ihl_bytes) != 0) {
		kprintf("ip_rput: checksum fail\n");
		freemsg(mp);
		return 0;
	}

	uint32_t src_ip = ntohl32(h->src_ip);
	uint32_t dst_ip = ntohl32(h->dst_ip);
	uint8_t  proto  = h->proto;
	mp->b_rptr += ihl_bytes;
	ip_demux(proto, src_ip, dst_ip, mp);
	return 0;
}

/* ---- ip_send convenience helper ----------------------------------- */

int ip_send(uint32_t dst_ip, uint8_t proto, mblk_t *mp)
{
	if (!mp || !ip_ctl_sd) {
		if (mp) freemsg(mp);
		return -1;
	}
	mblk_t *meta = allocb(sizeof(struct ip_send_meta), 0);
	if (!meta) {
		freemsg(mp);
		return -1;
	}
	meta->b_datap->db_type = M_PROTO;
	struct ip_send_meta *m = (struct ip_send_meta *)meta->b_wptr;
	m->prim   = IP_T_SEND_REQ;
	m->proto  = proto;
	m->_pad[0] = m->_pad[1] = 0;
	m->dst_ip = dst_ip;
	meta->b_wptr += sizeof(*m);
	meta->b_cont  = mp;
	return putnext(ip_ctl_sd->sd_wq, meta);
}

/* ---- streamtab ---------------------------------------------------- */

static struct module_info ip_minfo = {
	.mi_idnum  = 600,
	.mi_idname = "ip",
	.mi_minpsz = 0,
	.mi_maxpsz = 65535,
	.mi_hiwat  = 65536,
	.mi_lowat  = 32768,
};

static struct qinit ip_rinit = {
	.qi_putp  = ip_rput,
	.qi_minfo = &ip_minfo,
};
static struct qinit ip_winit = {
	.qi_putp  = ip_wput,
	.qi_minfo = &ip_minfo,
};
struct streamtab ip_streamtab = {
	.st_rdinit = &ip_rinit,
	.st_wrinit = &ip_winit,
};

/* ---- boot wiring -------------------------------------------------- */

extern void lo_init(void);

static int ip_boot_link_each(struct netif *n, void *arg)
{
	(void)arg;
	if (!n->streamtab) {
		kprintf("ip: skip %s -- no streamtab\n", n->name);
		return 0;
	}
	struct stdata *netif_sd = stream_build_kernel(n->streamtab,
	                                              n->name, 0);
	if (!netif_sd) {
		kprintf("ip: build_kernel(%s) failed\n", n->name);
		return 0;
	}
	long muxid = stream_ilink(ip_ctl_sd, netif_sd);
	if (muxid <= 0) {
		kprintf("ip: stream_ilink(%s) failed rc=%ld\n",
		        n->name, muxid);
		stream_destroy_kernel(netif_sd);
		return 0;
	}
	ip_lower_register((int)muxid, n);
	kprintf("ip: linked %s muxid=%ld\n", n->name, muxid);
	return 0;
}

void ip_init(void)
{
	lo_init();

	ip_ctl_sd = stream_build_kernel(&ip_streamtab, "ip_ctl", 0);
	if (!ip_ctl_sd) {
		kprintf("ip: failed to build control stream\n");
		return;
	}

	netif_for_each(ip_boot_link_each, NULL);

	kprintf("net: ip stack up\n");
}

/* ---- boot selftest ------------------------------------------------ */

extern int  net_run_icmp_selftest(uint32_t dst, uint16_t seq);

void net_selftest(void)
{
	if (net_run_icmp_selftest(0x7f000001u, 1) < 0) {
		kprintf("net: SELFTEST FAIL\n");
		return;
	}
	kprintf("net: selftest PASS\n");
}

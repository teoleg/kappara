/*
 * uts/os/net/ipv4.c -- IPv4 as a STREAMS multiplexor driver
 *
 * After the N2b rewrite IP is no longer a pair of direct-call
 * functions sandwiched between STREAMS endpoints.  It's a real
 * SVR4-style multiplexor:
 *
 *   - At boot, one kernel-only stream is built over ip_streamtab
 *     and held by ip_ctl_sd.  Each netif registered via
 *     netif_register exposes a streamtab; for each, the boot path
 *     builds a per-netif stream and stream_ilink's it underneath
 *     ip_ctl_sd.  IP's wput records the lower's l_qbot + muxid at
 *     I_LINK time; ip_lower_register backfills the netif pointer
 *     so route lookup can match by IP/netmask.
 *
 *   - Sending: ip_send() builds an M_PROTO{ip_send_meta} +
 *     M_DATA(payload) pair and putnext's it into ip_ctl_sd's wq.
 *     IP's wput decodes the meta, prepends the IP header in the
 *     payload's reserved headroom, picks a lower by route, and
 *     putnext's the payload mblk into lower->qbot (the lower
 *     stream's head wq).  The M_PROTO mblk is freed.
 *
 *   - Receiving: each lower stream's read-side chain was rewired
 *     by I_LINK so its driver's putnext-up enters ip's drv_rq.
 *     IP's rput validates the header, strips it, and demuxes by
 *     proto byte via ip_dispatch_input (icmp_input / udp_input).
 *     ICMP and UDP become real STREAMS modules in N2c; for now
 *     they're direct callees so this commit is contained.
 *
 * Headroom discipline (unchanged):
 *   ip_send's caller allocates the payload mblk with IP_HDR_LEN
 *   bytes of headroom; ip_wput rewinds b_rptr to expose 20 bytes
 *   and fills the IPv4 header in place.  No allocation on the tx
 *   path.
 */

#include <stdint.h>

#include "kappara/icmp.h"
#include "kappara/ip.h"
#include "kappara/kmem.h"
#include "kappara/netif.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"
#include "kappara/string.h"
#include "kappara/udp.h"

/* Increment-by-one packet ID -- gives every datagram a unique 16-bit
 * tag without needing real entropy. */
static uint16_t ip_id_next;

/* RFC 1071 one's-complement sum.  Used for the IPv4 header AND for
 * ICMP body checksums (they're the same algorithm). */
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
	if (len) {
		sum += (uint32_t)(p[0] << 8);
	}
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

/* ---- Mux state ----------------------------------------------------- */
/*
 * Fixed-size lower table.  Real Solaris IP keeps a dynamic list of
 * `ill_t` (interface link layer) entries; at our scale a 4-slot
 * static array covers lo0 + slip0 + a future ethernet + a spare.
 * No locking yet: links happen at boot, never get rewritten at
 * runtime.  When we add hot-plug interfaces, ip_lock goes here.
 */

#define IP_MAX_LOWERS	4

static struct ip_lower {
	int           muxid;	/* 0 = slot free */
	queue_t      *qbot;	/* lower stream's head wq -- putnext sends INTO */
	struct netif *nif;	/* identity for route match (NULL until
				 * ip_lower_register backfills) */
} ip_lowers[IP_MAX_LOWERS];

/* The boot-time IP control stream.  Every kernel-side ip_send
 * routes through here so we never have to know which user stream
 * to use. */
static struct stdata *ip_ctl_sd;

/* Walk ip_lowers picking the longest-prefix-match netif. */
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
		/* Inline popcount via Kernighan's loop -- same trick
		 * netif.c uses; -mgeneral-regs-only would otherwise
		 * call out to libgcc's __popcountdi2. */
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

/* ---- IP driver: wput -- send side --------------------------------- */
/*
 * Three message types arrive on the write side:
 *   M_IOCTL{I_LINK / I_UNLINK}: record / release a lower link slot
 *   M_PROTO{ip_send_meta} + M_DATA(payload): outbound packet
 *   anything else: drop
 */

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

static int ip_wput_data(mblk_t *meta_mp)
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
		if ((mp->b_wptr - mp->b_rptr)
		    < (int)sizeof(struct ip_send_meta)) {
			freemsg(mp);
			return -1;
		}
		return ip_wput_data(mp);
	}
	freemsg(mp);
	return 0;
}

/* ---- IP driver: rput -- receive side ------------------------------ */
/*
 * Reached by putnext-from-lower after the I_LINK rewire.  The mblk's
 * b_rptr..b_wptr is a complete IPv4 datagram (lower drivers don't
 * strip anything).  We validate, strip, demux.
 */

void ip_dispatch_input(uint32_t src, uint32_t dst, uint8_t proto,
                       mblk_t *mp)
{
	switch (proto) {
	case IPPROTO_ICMP:
		icmp_input(NULL, src, dst, mp);
		return;
	case IPPROTO_UDP:
		udp_input(NULL, src, dst, mp);
		return;
	case IPPROTO_TCP:
		freemsg(mp);
		return;
	default:
		kprintf("ip: drop unknown proto %u\n", proto);
		freemsg(mp);
		return;
	}
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
	if (ip_checksum(h, ihl_bytes) != 0) {
		kprintf("ip_rput: checksum fail\n");
		freemsg(mp);
		return 0;
	}

	uint32_t src = ntohl32(h->src_ip);
	uint32_t dst = ntohl32(h->dst_ip);
	uint8_t  proto = h->proto;
	mp->b_rptr += ihl_bytes;
	ip_dispatch_input(src, dst, proto, mp);
	return 0;
}

/* ---- ip_send ------------------------------------------------------ */

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
	m->dst_ip = dst_ip;
	m->proto  = proto;
	m->_pad[0] = m->_pad[1] = m->_pad[2] = 0;
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
/*
 * Sends an ICMP echo to 127.0.0.1 and verifies the reply.  Identical
 * call pattern to the old selftest -- icmp_send_echo now routes
 * through the IP STREAMS multiplexor instead of the direct-call
 * ip_output.  lo0's wput loops back via putnext(OTHERQ) and the
 * rewired chain delivers the reply to IP's rput which dispatches
 * back into icmp_input on the same thread.
 */

void net_selftest(void)
{
	int ok = 1;
	static const char payload[] = "kappara/ping/v1";
	const uint16_t id  = 0xABCD;
	const uint16_t seq = 1;

	icmp_arm_waiter(id, seq);
	int rc = icmp_send_echo(0x7f000001u, id, seq,
	                        payload, sizeof(payload) - 1);
	if (rc < 0) {
		kprintf("net: SELFTEST FAIL send_echo rc=%d\n", rc);
		return;
	}

	if (icmp_wait_reply(id, seq, /*ms=*/100) < 0) {
		kprintf("net: SELFTEST FAIL no reply\n");
		ok = 0;
	}

	if (ok)
		kprintf("net: selftest PASS\n");
}

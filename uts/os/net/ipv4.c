/*
 * kernel/net/ipv4.c -- IPv4 header build/parse + dispatch
 *
 * What this file is
 * -----------------
 * The C-level entry points the rest of the stack uses:
 *   ip_output: prepend an IPv4 header and shove the mblk at the
 *              right netif's tx callback.
 *   ip_input:  validate, strip header, dispatch by proto byte.
 *
 * The two protocol hooks reachable from ip_input today are
 * icmp_input (echo reply auto-handling) and a placeholder for UDP
 * (next commit).  Adding a new transport = one switch arm.
 *
 * No fragmentation, no IP options.  Outgoing packets get IHL=5,
 * TOS=0, frag_off=0 (DF flag would be 0x4000 -- we leave it clear so
 * a future SLIP path can frag), TTL=64.
 */

#include <stdint.h>

#include "kappara/icmp.h"
#include "kappara/ip.h"
#include "kappara/netif.h"
#include "kappara/printk.h"
#include "kappara/streams.h"
#include "kappara/string.h"

/* Increment-by-one packet ID -- gives every datagram a unique 16-bit
 * tag without needing real entropy. */
static uint16_t ip_id_next;

/* RFC 1071 one's-complement sum.  Used for the IP header AND for
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
		/* trailing odd byte goes in the high half of a word */
		sum += (uint32_t)(p[0] << 8);
	}
	/* Fold carries until none left, then one's-complement. */
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

int ip_output(uint32_t dst_ip, uint8_t proto, mblk_t *mp)
{
	if (!mp) return -1;

	struct netif *nif = netif_route(dst_ip);
	if (!nif) {
		kprintf("ip_output: no route to %u.%u.%u.%u\n",
			(dst_ip >> 24) & 0xff, (dst_ip >> 16) & 0xff,
			(dst_ip >>  8) & 0xff,  dst_ip        & 0xff);
		freemsg(mp);
		return -1;
	}

	unsigned payload = (unsigned)(mp->b_wptr - mp->b_rptr);
	if (payload > nif->mtu - IP_HDR_LEN) {
		kprintf("ip_output: payload %u > MTU %u (no frag)\n",
			payload, nif->mtu - IP_HDR_LEN);
		freemsg(mp);
		return -1;
	}

	/* Make room for the 20-byte header by sliding b_rptr back.  We
	 * allocated the mblk with HEADROOM elsewhere; just use it. */
	if ((size_t)(mp->b_rptr - mp->b_datap->db_base) < IP_HDR_LEN) {
		kprintf("ip_output: no IP headroom\n");
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
	h->proto     = proto;
	h->checksum  = 0;
	h->src_ip    = htonl32(nif->ip);
	h->dst_ip    = htonl32(dst_ip);
	uint16_t cs  = ip_checksum(h, IP_HDR_LEN);
	/* checksum field on the wire is big-endian; ip_checksum already
	 * returns the value as a 16-bit word that we just need to store
	 * in network order */
	h->checksum  = htons16(cs);

	return nif->tx(nif, mp);
}

void ip_input(struct netif *nif, mblk_t *mp)
{
	(void)nif;
	if (!mp) return;
	unsigned len = (unsigned)(mp->b_wptr - mp->b_rptr);
	if (len < IP_HDR_LEN) {
		kprintf("ip_input: runt %u bytes\n", len);
		freemsg(mp);
		return;
	}

	struct ip_hdr *h = (struct ip_hdr *)mp->b_rptr;
	if ((h->ver_ihl >> 4) != 4) {
		kprintf("ip_input: not IPv4 (ver_ihl=0x%x)\n", h->ver_ihl);
		freemsg(mp);
		return;
	}
	unsigned ihl_bytes = (unsigned)((h->ver_ihl & 0xf) * 4);
	if (ihl_bytes < IP_HDR_LEN || ihl_bytes > len) {
		kprintf("ip_input: bad IHL %u\n", ihl_bytes);
		freemsg(mp);
		return;
	}
	uint16_t total = ntohs16(h->total_len);
	if (total > len) {
		kprintf("ip_input: total_len %u > buf %u\n", total, len);
		freemsg(mp);
		return;
	}

	uint16_t got_cs = ip_checksum(h, ihl_bytes);
	if (got_cs != 0) {
		kprintf("ip_input: checksum fail (got=0x%04x)\n", got_cs);
		freemsg(mp);
		return;
	}

	uint32_t src = ntohl32(h->src_ip);
	uint32_t dst = ntohl32(h->dst_ip);
	uint8_t  proto = h->proto;

	/* Strip header.  Transport gets to see only its own payload. */
	mp->b_rptr += ihl_bytes;

	switch (proto) {
	case IPPROTO_ICMP:
		icmp_input(nif, src, dst, mp);
		return;
	case IPPROTO_UDP:
	case IPPROTO_TCP:
		/* not yet -- next commit */
		freemsg(mp);
		return;
	default:
		kprintf("ip_input: drop unknown proto %u\n", proto);
		freemsg(mp);
		return;
	}
}

extern void lo_init(void);

void ip_init(void)
{
	lo_init();
	kprintf("net: ip stack up\n");
}

/* ---- boot selftest ------------------------------------------------ */
/*
 * Sends an ICMP echo to 127.0.0.1 with a known payload and verifies
 * the reply arrives back through ip_input.  lo0's tx is synchronous,
 * so by the time icmp_send_echo returns the reply has already been
 * delivered (icmp_input on the reply path stashes id+seq into the
 * waiter slot).
 */

extern int  icmp_wait_reply(uint16_t, uint16_t, unsigned);
extern void icmp_arm_waiter(uint16_t id, uint16_t seq);

void net_selftest(void)
{
	int ok = 1;
	static const char payload[] = "kappara/ping/v1";
	const uint16_t id  = 0xABCD;
	const uint16_t seq = 1;

	/* lo0 loops synchronously -- the reply arrives BEFORE
	 * icmp_send_echo returns.  Arm the waiter slot first so the
	 * reply has somewhere to land. */
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

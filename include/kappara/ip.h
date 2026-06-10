/*
 * include/kappara/ip.h -- IPv4 layer
 *
 * Header format (RFC 791) and the two entry points the rest of the
 * stack uses:
 *
 *   ip_output(dst, proto, mp)   take a payload mblk and a destination
 *                                IP + proto, route to a netif, prepend
 *                                IPv4 header, call netif->tx.
 *
 *   ip_input(nif, mp)           a netif's rx path delivers an mblk
 *                                whose b_rptr..b_wptr is a complete
 *                                IPv4 datagram.  We verify the
 *                                checksum + version, strip the header,
 *                                dispatch by proto byte.
 *
 * Following the SVR4/Solaris convention, in-kernel C code uses
 * HOST byte order for addresses; the header writers and parsers
 * htonl/ntohl at the wire boundary.  This avoids endian errors
 * propagating through the routing logic.
 *
 * No fragmentation, no options.  IHL is fixed at 5 (20 bytes).
 * TTL hardcoded to 64.  Source IP for outgoing packets is the chosen
 * netif's IP.
 */

#ifndef KAPPARA_IP_H
#define KAPPARA_IP_H

#include <stdint.h>

struct netif;
struct msgb;
typedef struct msgb mblk_t;

#define IPPROTO_ICMP	1
#define IPPROTO_TCP	6
#define IPPROTO_UDP	17

#define IP_HDR_LEN	20	/* no options */
#define IP_DEFAULT_TTL	64

struct ip_hdr {
	uint8_t  ver_ihl;	/* 0x45 = IPv4 + IHL=5 */
	uint8_t  tos;
	uint16_t total_len;	/* big-endian on the wire */
	uint16_t id;
	uint16_t frag_off;
	uint8_t  ttl;
	uint8_t  proto;
	uint16_t checksum;
	uint32_t src_ip;	/* big-endian on the wire */
	uint32_t dst_ip;
} __attribute__((packed));

/* Internet checksum (RFC 1071): one's-complement sum of 16-bit words.
 * Used for the IPv4 header AND the ICMP body; same algorithm for both. */
uint16_t ip_checksum(const void *buf, unsigned len);

/* Endianness helpers.  AArch64 is little-endian; "network byte order"
 * is big-endian.  These are constexpr-friendly so callers can use them
 * in initialisers if they want. */
static inline uint16_t htons16(uint16_t v)
{
	return (uint16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff));
}
static inline uint32_t htonl32(uint32_t v)
{
	return ((v & 0xff)       << 24) |
	       ((v & 0xff00)     <<  8) |
	       ((v & 0xff0000)   >>  8) |
	       ((v & 0xff000000) >> 24);
}
#define ntohs16 htons16
#define ntohl32 htonl32

/* ---- IP-as-STREAMS-multiplexor send metadata -----------------------
 *
 * Callers send M_PROTO{struct ip_send_meta} + M_DATA(payload) to IP's
 * write side.  IP's wput decodes the meta, prepends the 20-byte IPv4
 * header to the payload, routes by netmask to one of its I_LINKed
 * lower streams (one per netif), and putnexts the rewritten payload
 * down.  ip_send() is the convenience helper that builds the M_PROTO
 * + M_DATA pair and putnexts into the IP driver's stream.
 */
struct ip_send_meta {
	uint32_t dst_ip;	/* host byte order */
	uint8_t  proto;
	uint8_t  _pad[3];
};

/* Build M_PROTO{ip_send_meta} + M_DATA(payload), putnext into IP.
 * mp's b_rptr..b_wptr is the L4 payload (with IP_HDR_LEN headroom
 * reserved at b_rptr -- IP's wput rewinds to fill the header in
 * place).  Returns 0 on success, -1 on alloc/route failure (mp
 * freed either way). */
int  ip_send (uint32_t dst_ip, uint8_t proto, mblk_t *mp);

/* Called by IP driver's rput once the IP header has been validated
 * and stripped.  Dispatches by proto byte to icmp_input / udp_input.
 * Lives here (not file-static in ipv4.c) so unit-style selftests can
 * inject mblks straight into the demux. */
void ip_dispatch_input(uint32_t src, uint32_t dst, uint8_t proto,
                       mblk_t *mp);

/* Register a netif's identity against its IP-multiplexor link slot
 * after stream_ilink has succeeded.  IP's wput populates the slot's
 * qbot at I_LINK time; this fills in the nif pointer so route lookup
 * can match a destination against the lower's IP/netmask. */
void ip_lower_register(int muxid, struct netif *nif);

/* IP driver's streamtab.  Built into one kernel stream at boot for
 * the control plane; each netif's stream gets I_LINKed under it. */
extern struct streamtab ip_streamtab;

/* Init (called once from kmain after streams_head_init).  Brings up
 * the IP driver stream + walks the netif registry, building one
 * kernel stream per netif and I_LINKing each underneath. */
void ip_init  (void);

/* Boot-time selftest -- sends an ICMP echo via 127.0.0.1, waits for
 * the reply on a small internal queue, kprintfs PASS/FAIL. */
void net_selftest(void);

#endif

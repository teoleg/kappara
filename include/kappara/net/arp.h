/*
 * include/kappara/net/arp.h -- shared ARP cache (arp(7P)-flavoured)
 *
 * One kernel-wide cache replacing the per-driver single-entry
 * gateway-MAC fields.  Not (yet) an autopushed STREAMS module the
 * way Solaris pairs arp with ip; the arpif ops vector is shaped so
 * it could become one without changing the drivers again.  See
 * docs/DLPI.md.
 *
 * Drivers:
 *   - arp_ifattach() once at init with their netif, MAC and a raw
 *     frame-transmit callback;
 *   - arp_input() for every received Ethernet frame whose ethertype
 *     is 0x0806 (full frame, Ethernet header included);
 *   - arp_resolve() to map next-hop IPv4 -> MAC on the TX path.
 */

#ifndef KAPPARA_NET_ARP_H
#define KAPPARA_NET_ARP_H

#include <stdint.h>

struct netif;

struct arpif {
	struct netif *nif;
	const uint8_t *mac;	/* our station address (6 bytes, driver-owned) */
	int (*tx)(void *cookie, const void *frame, unsigned len);
	void *cookie;
};

void arp_ifattach(struct arpif *aif);

/* Process one received ARP frame (replies to requests for our IP;
 * learns sender mappings).  Safe to call for any frame that carried
 * ethertype 0x0806. */
void arp_input(struct arpif *aif, const uint8_t *frame, unsigned len);

/* Resolve the next hop for a fully-built Ethernet frame whose dst
 * MAC (frame[0..5]) is still blank.  On a cache hit: writes the MAC
 * into the frame and returns 1 -- caller transmits.  On a miss:
 * parks the frame in a hold slot, broadcasts an ARP REQUEST, and
 * returns 0 -- the module transmits the held frame itself when the
 * reply arrives (classic BSD one-packet hold queue).  Without the
 * hold, the first packet to every new destination was silently
 * dropped, which one-shot clients (DNS lookups) never recover from. */
int arp_resolve_hold(struct arpif *aif, uint32_t ip,
                     uint8_t *frame, unsigned len);

/* /proc/arp snapshot support. */
struct arp_view {
	uint32_t    ip;
	uint8_t     mac[6];
	const char *ifname;
	unsigned    age_s;
};
int arp_for_each(int (*cb)(const struct arp_view *v, void *arg), void *arg);

#endif

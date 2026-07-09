/*
 * uts/os/net/arp.c -- shared ARP cache
 *
 * See include/kappara/net/arp.h and docs/DLPI.md for the design.
 *
 * The cache is a small fixed table (ARP_CACHE_SIZE entries, LRU-ish
 * replacement by oldest stamp).  Entries age; a hit older than
 * ARP_STALE_S still returns the cached MAC but re-broadcasts a
 * REQUEST so the entry refreshes in the background -- traffic keeps
 * flowing across a router MAC change with at most one stale-MAC
 * window.  A learned MAC *change* for a cached IP is logged; that's
 * the observability the single-field-per-driver scheme never had.
 *
 * Locking: one spinlock around the table.  arp_input runs from RX
 * kthreads, arp_resolve from arbitrary TX contexts; both are short
 * critical sections (no allocation, no TX under the lock).
 */

#include <stdint.h>

#include "kappara/core/printk.h"
#include "kappara/core/spinlock.h"
#include "kappara/core/string.h"
#include "kappara/net/arp.h"
#include "kappara/net/netif.h"

#define ARP_CACHE_SIZE	16
#define ARP_STALE_S	300u

#define ARP_HW_ETHER	1
#define ARP_PROTO_IPV4	0x0800
#define ARP_OP_REQUEST	1
#define ARP_OP_REPLY	2
#define ETH_HDR_LEN	14
#define ETHERTYPE_ARP	0x0806

struct arp_wire {
	uint16_t hw_type;
	uint16_t proto_type;
	uint8_t  hw_len;
	uint8_t  proto_len;
	uint16_t opcode;
	uint8_t  sender_mac[6];
	uint8_t  sender_ip[4];
	uint8_t  target_mac[6];
	uint8_t  target_ip[4];
} __attribute__((packed));

struct arp_entry {
	uint32_t ip;		/* host order; 0 = free slot */
	uint8_t  mac[6];
	const char *ifname;
	uint64_t stamp;		/* cntpct at learn/refresh time */
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static spinlock_t       arp_lock = SPINLOCK_INIT;

static uint64_t now_ticks(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static uint64_t ticks_per_s(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v));
	return v ? v : 1;
}

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}
static uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
	     | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

void arp_ifattach(struct arpif *aif)
{
	(void)aif;	/* registration is currently implicit -- the aif
			 * travels with every call.  The hook exists so a
			 * future arp STREAMS module has a plumb point. */
}

/* Insert or refresh ip->mac.  Logs when a known IP changes MAC. */
static void arp_learn(uint32_t ip, const uint8_t mac[6], const char *ifname)
{
	if (!ip) return;

	unsigned long f = spin_lock_irq_save(&arp_lock);
	struct arp_entry *slot = 0, *oldest = &arp_cache[0];
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		struct arp_entry *e = &arp_cache[i];
		if (e->ip == ip) { slot = e; break; }
		if (!e->ip) { if (!slot) slot = e; continue; }
		if (e->stamp < oldest->stamp) oldest = e;
	}
	if (!slot) slot = oldest;

	int changed = (slot->ip == ip) && kmemcmp(slot->mac, mac, 6) != 0;
	uint8_t old[6];
	if (changed) kmemcpy(old, slot->mac, 6);

	slot->ip     = ip;
	kmemcpy(slot->mac, mac, 6);
	slot->ifname = ifname;
	slot->stamp  = now_ticks();
	spin_unlock_irq_restore(&arp_lock, f);

	if (changed)
		kprintf("arp: %u.%u.%u.%u moved "
			"%02x:%02x:%02x:%02x:%02x:%02x -> "
			"%02x:%02x:%02x:%02x:%02x:%02x\n",
			(ip >> 24) & 0xff, (ip >> 16) & 0xff,
			(ip >>  8) & 0xff,  ip        & 0xff,
			old[0], old[1], old[2], old[3], old[4], old[5],
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Look up ip.  Returns 1 on hit (fills mac_out; *stale set when the
 * entry is older than ARP_STALE_S), 0 on miss. */
static int arp_lookup(uint32_t ip, uint8_t mac_out[6], int *stale)
{
	int hit = 0;
	unsigned long f = spin_lock_irq_save(&arp_lock);
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		struct arp_entry *e = &arp_cache[i];
		if (e->ip != ip) continue;
		kmemcpy(mac_out, e->mac, 6);
		*stale = (now_ticks() - e->stamp) >
			 (uint64_t)ARP_STALE_S * ticks_per_s();
		hit = 1;
		break;
	}
	spin_unlock_irq_restore(&arp_lock, f);
	return hit;
}

static void arp_tx(struct arpif *aif, uint16_t op,
		   const uint8_t dst_mac[6], uint32_t target_ip,
		   const uint8_t target_mac[6])
{
	uint8_t frame[ETH_HDR_LEN + sizeof(struct arp_wire)];

	kmemcpy(frame, dst_mac, 6);
	kmemcpy(frame + 6, aif->mac, 6);
	put_be16(frame + 12, ETHERTYPE_ARP);

	struct arp_wire *a = (struct arp_wire *)(frame + ETH_HDR_LEN);
	put_be16((uint8_t *)&a->hw_type,    ARP_HW_ETHER);
	put_be16((uint8_t *)&a->proto_type, ARP_PROTO_IPV4);
	a->hw_len    = 6;
	a->proto_len = 4;
	put_be16((uint8_t *)&a->opcode, op);
	kmemcpy(a->sender_mac, aif->mac, 6);
	put_be32(a->sender_ip, aif->nif ? aif->nif->ip : 0);
	if (target_mac)
		kmemcpy(a->target_mac, target_mac, 6);
	else
		kmemset(a->target_mac, 0, 6);
	put_be32(a->target_ip, target_ip);

	aif->tx(aif->cookie, frame, sizeof(frame));
}

void arp_input(struct arpif *aif, const uint8_t *frame, unsigned len)
{
	if (len < ETH_HDR_LEN + sizeof(struct arp_wire)) return;
	const struct arp_wire *a =
		(const struct arp_wire *)(frame + ETH_HDR_LEN);
	if (get_be16((const uint8_t *)&a->hw_type)    != ARP_HW_ETHER)  return;
	if (get_be16((const uint8_t *)&a->proto_type) != ARP_PROTO_IPV4) return;

	uint16_t op  = get_be16((const uint8_t *)&a->opcode);
	uint32_t sip = get_be32(a->sender_ip);
	uint32_t tip = get_be32(a->target_ip);
	const char *ifname = aif->nif ? aif->nif->name : "?";

	/* Learn the sender from both requests and replies (a
	 * gratuitous REQUEST is how peers announce a MAC change). */
	arp_learn(sip, a->sender_mac, ifname);

	if (op == ARP_OP_REQUEST && aif->nif && tip == aif->nif->ip
	    && aif->nif->ip != 0)
		arp_tx(aif, ARP_OP_REPLY, a->sender_mac, sip, a->sender_mac);
}

int arp_resolve(struct arpif *aif, uint32_t ip, uint8_t mac_out[6])
{
	static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
	int stale = 0;

	if (!ip) return 0;
	if (arp_lookup(ip, mac_out, &stale)) {
		/* Serve stale entries but refresh in the background so
		 * a router failover converges without dropping flows. */
		if (stale)
			arp_tx(aif, ARP_OP_REQUEST, bcast, ip, 0);
		return 1;
	}
	arp_tx(aif, ARP_OP_REQUEST, bcast, ip, 0);
	return 0;
}

int arp_for_each(int (*cb)(const struct arp_view *v, void *arg), void *arg)
{
	/* Snapshot under the lock, render outside it -- cb prints. */
	struct arp_view snap[ARP_CACHE_SIZE];
	int n = 0;

	unsigned long f = spin_lock_irq_save(&arp_lock);
	uint64_t now = now_ticks(), per_s = ticks_per_s();
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		struct arp_entry *e = &arp_cache[i];
		if (!e->ip) continue;
		snap[n].ip = e->ip;
		kmemcpy(snap[n].mac, e->mac, 6);
		snap[n].ifname = e->ifname ? e->ifname : "?";
		snap[n].age_s  = (unsigned)((now - e->stamp) / per_s);
		n++;
	}
	spin_unlock_irq_restore(&arp_lock, f);

	for (int i = 0; i < n; i++) {
		int r = cb(&snap[i], arg);
		if (r) return r;
	}
	return 0;
}

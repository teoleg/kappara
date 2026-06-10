/*
 * uts/os/net/netif.c -- network interface registry
 *
 * Carries the identity of each registered interface (name, IP,
 * netmask, MTU, streamtab) so IP's mux can route by netmask after
 * building one kernel stream per netif and I_LINKing it underneath.
 *
 * Walks a singly-linked list; we never have more than a handful of
 * interfaces.  The longest-prefix-match `netif_route` helper isn't
 * used by IP itself anymore (IP routes through ip_lowers[] which
 * carries the netif pointer per linked stream) but is exposed for
 * future callers that want the bare lookup.
 */

#include <stdint.h>

#include "kappara/netif.h"
#include "kappara/printk.h"

static struct netif *netif_list;

/* AArch64 with -mgeneral-regs-only can't use NEON CNT for popcount,
 * and gcc emits a libgcc call (__popcountdi2) that we don't link.
 * Kernighan's loop -- O(set bits) -- handles the four-and-change set
 * bits in a typical /8 mask just fine. */
static unsigned popcount32(uint32_t v)
{
	unsigned c = 0;
	while (v) { v &= v - 1; c++; }
	return c;
}

void netif_register(struct netif *nif)
{
	if (!nif) return;
	nif->next = netif_list;
	netif_list = nif;
	kprintf("net: registered %s ip=%u.%u.%u.%u/%u mtu=%u\n",
		nif->name,
		(nif->ip >> 24) & 0xff,
		(nif->ip >> 16) & 0xff,
		(nif->ip >>  8) & 0xff,
		(nif->ip      ) & 0xff,
		popcount32(nif->netmask),
		nif->mtu);
}

struct netif *netif_route(uint32_t dst_ip)
{
	/* Longest-prefix match isn't strictly necessary with a handful
	 * of subnets, but doing it makes the logic generalise when SLIP
	 * point-to-point and Ethernet broadcast routes coexist. */
	struct netif *best = 0;
	unsigned best_prefix = 0;
	for (struct netif *n = netif_list; n; n = n->next) {
		if ((dst_ip & n->netmask) == (n->ip & n->netmask)) {
			unsigned p = (unsigned)popcount32(n->netmask);
			if (!best || p >= best_prefix) {
				best = n;
				best_prefix = p;
			}
		}
	}
	return best;
}

int netif_for_each(int (*cb)(struct netif *nif, void *arg), void *arg)
{
	for (struct netif *n = netif_list; n; n = n->next) {
		int r = cb(n, arg);
		if (r) return r;
	}
	return 0;
}

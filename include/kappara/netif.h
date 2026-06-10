/*
 * include/kappara/netif.h -- network interface (DLPI-flavoured)
 *
 * A `netif` is the bottom of the kappara network stack: the thing
 * that actually puts bytes "on the wire" (or, for lo0, into a queue
 * back into the stack itself).  Loosely modelled on SVR4's DLPI
 * (Data Link Provider Interface) -- DLPI is the streams-shaped
 * driver API for network devices.  We don't ship the full DLPI
 * primitive set (DL_BIND_REQ, DL_UNITDATA_REQ, ...); just the C
 * shape the kernel-side stack needs.
 *
 * Each netif owns a name, an IPv4 address + netmask, an MTU, and a
 * `tx` callback the IP layer calls to send.  Drivers register
 * themselves via `netif_register` from their *_init function.
 * Routing is netmask-walked: `netif_route(dst)` finds the interface
 * whose subnet contains `dst`.
 *
 * Packet shape on the netif boundary: an mblk_t whose
 * b_rptr..b_wptr is a complete IP datagram (no link-layer header,
 * since lo0 doesn't have one; SLIP and Ethernet drivers will frame
 * inside their own tx/rx).
 */

#ifndef KAPPARA_NETIF_H
#define KAPPARA_NETIF_H

#include <stdint.h>

struct msgb;
typedef struct msgb mblk_t;
struct streamtab;

struct netif {
	const char *name;	/* "lo0", "slip0", ... -- not freed; static */
	uint32_t    ip;		/* host byte order, e.g. 127.0.0.1 = 0x7f000001 */
	uint32_t    netmask;	/* host byte order, e.g. /8 = 0xff000000 */
	unsigned    mtu;	/* bytes -- IP datagram cap */
	/* The driver's STREAMS personality.  IP (the multiplexor) builds
	 * a kernel stream from this and I_LINKs it underneath at boot.
	 * The qi_putp on the write side is the "tx" entry point now --
	 * IP's wput putnexts mblks into it. */
	struct streamtab *streamtab;
	/* Legacy direct-call tx -- unused after the N2b STREAMS migration
	 * but kept on the struct so future drivers that never grew a
	 * streamtab (e.g., a one-off pseudo-iface) could still fit. */
	int       (*tx)(struct netif *nif, mblk_t *mp);
	struct netif *next;	/* registry linkage; netif_register sets */
};

void          netif_register(struct netif *nif);
struct netif *netif_route   (uint32_t dst_ip);

/* Walk the registry in registration order (newest first).  cb returns
 * non-zero to stop iteration; netif_for_each returns that value. */
int           netif_for_each(int (*cb)(struct netif *nif, void *arg),
                             void *arg);

#endif

/*
 * kernel/net/lo.c -- loopback interface
 *
 * Dual personality, classic SVR4:
 *
 *   - Internal: registers a `struct netif` (name "lo0", ip 127.0.0.1)
 *     with the netif registry, so ip_output can route there.  The
 *     netif's tx callback feeds the same mblk back into ip_input
 *     via netif_input.  Synchronous loopback: caller's stack sees
 *     the full round-trip before tx returns.
 *
 *   - User-facing: registers as a STREAMS cdev /dev/lo0.  Open it
 *     and you get a stream that talks raw IP datagrams to lo0.
 *     Writing an mblk = "transmit packet"; the corresponding rx
 *     side delivers any packet sent to lo0's IP via the IP layer.
 *     Reserved for future use (kernel selftest doesn't need it).
 *
 * No link-layer header.  The mblk's b_rptr..b_wptr at the netif
 * boundary IS the full IP datagram.
 *
 * Synchronous vs deferred
 * -----------------------
 * Real network drivers don't loop synchronously -- they enqueue an
 * mblk on an rx queue and an upcall thread later calls ip_input.
 * For lo0 we COULD use the streams srvp deferred mechanism, but for
 * the boot selftest a synchronous round-trip is cleaner: by the
 * time `icmp_send_echo` returns, the reply has already been
 * delivered.  The recursion depth is bounded by the number of
 * ICMP exchanges we're chaining (1 round-trip = 2 deep).
 */

#include <stdint.h>

#include "kappara/cdevsw.h"
#include "kappara/ip.h"
#include "kappara/netif.h"
#include "kappara/printk.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"

/* ---- netif side --------------------------------------------------- */

static struct netif lo0_nif;

static int lo_tx(struct netif *nif, mblk_t *mp)
{
	/* Loopback: hand the same mblk straight back to ip_input.
	 * netif_input -> ip_input takes ownership and freemsg's at the
	 * end of processing. */
	netif_input(nif, mp);
	return 0;
}

/* ---- /dev/lo0 STREAMS driver -------------------------------------- */
/*
 * Open a stream to inject and receive raw IP packets on lo0.  Bytes
 * written down the stream are taken as a complete IP datagram and
 * handed to ip_output via lo_tx.  Bytes coming up come from
 * ip_input dispatching to "user receiver" -- not wired yet, so this
 * direction is reserved.
 */

static int lo_drv_wq_putp(queue_t *q, mblk_t *mp)
{
	(void)q;
	if (!mp) return 0;
	if (mp->b_datap->db_type != M_DATA) {
		freemsg(mp);
		return 0;
	}
	/* The mblk is an IP datagram.  Inject it as if it had just
	 * arrived on lo0.  IP layer takes ownership. */
	netif_input(&lo0_nif, mp);
	return 0;
}

static int lo_drv_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static struct module_info lo_drv_minfo = {
	.mi_idnum  = 400,
	.mi_idname = "lo0",
	.mi_minpsz = 0,
	.mi_maxpsz = 1500,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit lo_drv_rinit = {
	.qi_putp = lo_drv_rq_putp, .qi_minfo = &lo_drv_minfo,
};
static struct qinit lo_drv_winit = {
	.qi_putp = lo_drv_wq_putp, .qi_minfo = &lo_drv_minfo,
};

static struct streamtab lo_streamtab = {
	.st_rdinit = &lo_drv_rinit,
	.st_wrinit = &lo_drv_winit,
};

/* ---- init --------------------------------------------------------- */

void lo_init(void)
{
	lo0_nif.name    = "lo0";
	lo0_nif.ip      = 0x7f000001u;	/* 127.0.0.1 */
	lo0_nif.netmask = 0xff000000u;	/* /8 */
	lo0_nif.mtu     = 1500;
	lo0_nif.tx      = lo_tx;
	netif_register(&lo0_nif);

	streams_register("lo0", &lo_streamtab);
	cdev_register(CDEV_MAJ_LO, "lo0", &lo_streamtab);
}

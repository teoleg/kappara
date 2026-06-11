/*
 * uts/os/net/lo.c -- loopback interface as a leaf STREAMS driver.
 *
 * After the N2b migration lo0 is a pure SVR4 STREAMS driver.  Its
 * write-side qi_putp loops the mblk straight back up via
 *
 *     putnext(OTHERQ(q), mp);
 *
 * which would normally hit the lo0 stream's own head-rq.  But this
 * stream is built at boot by ip_init() and immediately I_LINKed
 * underneath IP -- so the I_LINK rewire points OTHERQ(q)->q_next
 * at IP's drv_rq instead.  Loopback packets land in IP's rput which
 * validates the header and demuxes by proto byte.  No netif_input
 * callback, no direct ip_input call.  Pure putnext.
 *
 * lo0 still registers a struct netif so IP can find it for routing
 * (the 127.0.0.0/8 prefix match).  The netif's `tx` callback is
 * NULL: nothing calls it after the rewrite.  Identity (name, IP,
 * netmask, MTU, streamtab pointer) is what the netif carries now;
 * the data path is queues end-to-end.
 */

#include <stdint.h>

#include "kappara/net/ip.h"
#include "kappara/net/netif.h"
#include "kappara/core/printk.h"
#include "kappara/io/streams.h"

/* ---- STREAMS driver --------------------------------------------- */

static int lo_wq_putp(queue_t *q, mblk_t *mp)
{
	if (!mp) return 0;
	if (mp->b_datap->db_type != M_DATA) {
		freemsg(mp);
		return 0;
	}
	/* OTHERQ(q) is the read side of this driver.  After I_LINK its
	 * q_next is IP's drv_rq -- so the packet flows up into IP for
	 * demux.  Before linking it would hit the lo0 stream-head's rq
	 * and just queue there; the linker (ip_init) guarantees we're
	 * linked before anyone sends to us. */
	return putnext(OTHERQ(q), mp);
}

static int lo_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static struct module_info lo_minfo = {
	.mi_idnum  = 400,
	.mi_idname = "lo0",
	.mi_minpsz = 0,
	.mi_maxpsz = 1500,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit lo_rinit = {
	.qi_putp = lo_rq_putp, .qi_minfo = &lo_minfo,
};
static struct qinit lo_winit = {
	.qi_putp = lo_wq_putp, .qi_minfo = &lo_minfo,
};
static struct streamtab lo_streamtab = {
	.st_rdinit = &lo_rinit,
	.st_wrinit = &lo_winit,
};

/* ---- netif registration ----------------------------------------- */

static struct netif lo0_nif;

void lo_init(void)
{
	lo0_nif.name     = "lo0";
	lo0_nif.ip       = 0x7f000001u;	/* 127.0.0.1 */
	lo0_nif.netmask  = 0xff000000u;	/* /8 */
	lo0_nif.mtu      = 1500;
	lo0_nif.streamtab = &lo_streamtab;
	lo0_nif.tx       = NULL;	/* unused after STREAMS migration */
	netif_register(&lo0_nif);
}

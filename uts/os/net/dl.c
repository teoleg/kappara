/*
 * uts/os/net/dl.c -- raw datalink provider (/dev/eth0, mini-DLPI)
 *
 * Shared helper so Ethernet drivers don't each grow their own raw
 * access path.  A driver calls dl_register() once at init (name,
 * mac, mtu, tx callback) and dl_input() for every received frame.
 * Userland (dhcpagent, tcpdump-alikes) opens /dev/<name>, does the
 * DL_INFO / DL_BIND handshake over putmsg/getmsg, then reads and
 * writes complete Ethernet frames.  See net/dlpi.h + docs/DLPI.md.
 *
 * Fan-out: every open stream whose bound SAP matches the frame's
 * ethertype (or 0 = all) gets its own copy.  The IP path is NOT
 * routed through here -- drivers still putnext IP payloads up
 * their I_LINKed stream directly; dl taps see the raw frame too.
 *
 * Locking: one spinlock over the per-device open table.  dl_input
 * runs on driver RX kthreads; open/close on user threads.  allocb
 * + putnext happen outside the lock (snapshot the target queues).
 */

#include <stdint.h>

#include "kappara/core/printk.h"
#include "kappara/core/spinlock.h"
#include "kappara/core/string.h"
#include "kappara/fs/vfs.h"
#include "kappara/io/cdevsw.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/net/dl.h"
#include "kappara/net/dlpi.h"

#define DL_MAX_DEVS	2
#define DL_SAP_UNBOUND	0xffffffffu
#define DL_MAX_OPENS	2	/* concurrent raw streams per device */
#define DL_FRAME_MAX	1600

struct dl_open {
	queue_t *drv_rq;	/* driver-side read queue; putnext -> head */
	uint32_t sap;		/* 0 = all ethertypes */
	int      used;
};

struct dlif {
	const char    *name;
	const uint8_t *mac;
	unsigned       mtu;
	int          (*tx)(void *cookie, const void *frame, unsigned len);
	void          *cookie;
	unsigned       minor;
	struct dl_open opens[DL_MAX_OPENS];
};

static struct dlif dl_devs[DL_MAX_DEVS];
static unsigned    dl_ndevs;
static spinlock_t  dl_lock = SPINLOCK_INIT;

/* ---- STREAMS personality ------------------------------------------- */

static struct dlif *dl_by_minor(unsigned minor)
{
	return (minor < dl_ndevs) ? &dl_devs[minor] : 0;
}

static int dl_qopen(queue_t *rq)
{
	struct stdata *sd = rq->q_ptr;	/* stream_build backref */
	if (!sd) return -1;
	struct dlif *d = dl_by_minor(sd->sd_minor);
	if (!d) return -1;

	unsigned long f = spin_lock_irq_save(&dl_lock);
	struct dl_open *o = 0;
	for (int i = 0; i < DL_MAX_OPENS; i++)
		if (!d->opens[i].used) { o = &d->opens[i]; break; }
	if (!o) {
		spin_unlock_irq_restore(&dl_lock, f);
		return -1;
	}
	o->used   = 1;
	o->sap    = DL_SAP_UNBOUND;	/* DLPI: nothing arrives before
					 * DL_BIND (sap 0 = everything) */
	o->drv_rq = rq;
	spin_unlock_irq_restore(&dl_lock, f);

	/* Both queues of the pair see the open slot via q_ptr. */
	rq->q_ptr = o;
	if (rq->q_link) rq->q_link->q_ptr = o;
	return 0;
}

static int dl_qclose(queue_t *q)
{
	struct dl_open *o = q->q_ptr;
	if (o) {
		unsigned long f = spin_lock_irq_save(&dl_lock);
		o->used   = 0;
		o->drv_rq = 0;
		spin_unlock_irq_restore(&dl_lock, f);
	}
	return 0;
}

/* Reply to a DLPI M_PROTO request with an ACK M_PROTO going up. */
static void dl_reply(queue_t *wq, const void *buf, unsigned len)
{
	mblk_t *mp = allocb(len, 0);
	if (!mp) return;
	mp->b_datap->db_type = M_PROTO;
	kmemcpy(mp->b_wptr, buf, len);
	mp->b_wptr += len;
	putnext(OTHERQ(wq), mp);
}

static void dl_error_reply(queue_t *wq, uint32_t failed_prim)
{
	struct dl_error_ack ea = {
		.dl_primitive       = DL_ERROR_ACK,
		.dl_error_primitive = failed_prim,
	};
	dl_reply(wq, &ea, sizeof(ea));
}

static int dl_wq_putp(queue_t *q, mblk_t *mp)
{
	struct dl_open *o = q->q_ptr;
	struct dlif    *d = 0;
	if (o) {
		/* Recover the dlif from the open slot's position. */
		for (unsigned i = 0; i < dl_ndevs; i++)
			if (o >= dl_devs[i].opens
			 && o <  dl_devs[i].opens + DL_MAX_OPENS)
				d = &dl_devs[i];
	}
	if (!o || !d || !mp) {
		freemsg(mp);
		return -1;
	}

	if (mp->b_datap->db_type == M_PROTO) {
		unsigned n = (unsigned)(mp->b_wptr - mp->b_rptr);
		uint32_t prim = (n >= 4) ? *(uint32_t *)mp->b_rptr : 0xffu;

		if (prim == DL_INFO_REQ) {
			struct dl_info_ack ia;
			kmemset(&ia, 0, sizeof(ia));
			ia.dl_primitive = DL_INFO_ACK;
			ia.dl_mtu       = d->mtu;
			ia.dl_sap       = o->sap;
			kmemcpy(ia.dl_mac, d->mac, 6);
			dl_reply(q, &ia, sizeof(ia));
		} else if (prim == DL_BIND_REQ
		        && n >= sizeof(struct dl_bind_req)) {
			struct dl_bind_req *br = (struct dl_bind_req *)mp->b_rptr;
			o->sap = br->dl_sap;
			struct dl_bind_ack ba = {
				.dl_primitive = DL_BIND_ACK,
				.dl_sap       = o->sap,
			};
			dl_reply(q, &ba, sizeof(ba));
		} else {
			dl_error_reply(q, prim);
		}
		freemsg(mp);
		return 0;
	}

	if (mp->b_datap->db_type != M_DATA) {
		freemsg(mp);
		return 0;
	}

	/* Raw frame TX: linearise the chain and hand it to the driver. */
	uint8_t frame[DL_FRAME_MAX];
	unsigned len = 0;
	for (mblk_t *m = mp; m; m = m->b_cont) {
		unsigned n = (unsigned)(m->b_wptr - m->b_rptr);
		if (len + n > sizeof(frame)) { len = 0; break; }
		kmemcpy(frame + len, m->b_rptr, n);
		len += n;
	}
	freemsg(mp);
	if (len < 14) return -1;	/* below Ethernet header: drop */
	return d->tx(d->cookie, frame, len);
}

static int dl_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static struct module_info dl_minfo = {
	.mi_idnum  = 2100,
	.mi_idname = "dl",
	.mi_minpsz = 0,
	.mi_maxpsz = DL_FRAME_MAX,
	.mi_hiwat  = 32768,
	.mi_lowat  = 8192,
};

static struct qinit dl_rinit = {
	.qi_putp   = dl_rq_putp,
	.qi_qopen  = dl_qopen,
	.qi_qclose = dl_qclose,
	.qi_minfo  = &dl_minfo,
};
static struct qinit dl_winit = {
	.qi_putp  = dl_wq_putp,
	.qi_minfo = &dl_minfo,
};
static struct streamtab dl_streamtab = {
	.st_rdinit = &dl_rinit,
	.st_wrinit = &dl_winit,
};

/* ---- Driver-facing API ---------------------------------------------- */

struct dlif *dl_register(const char *name, const uint8_t *mac, unsigned mtu,
			 int (*tx)(void *cookie, const void *frame,
			           unsigned len),
			 void *cookie)
{
	if (dl_ndevs >= DL_MAX_DEVS) {
		kprintf("dl: too many datalinks\n");
		return 0;
	}
	struct dlif *d = &dl_devs[dl_ndevs];
	d->name   = name;
	d->mac    = mac;
	d->mtu    = mtu;
	d->tx     = tx;
	d->cookie = cookie;
	d->minor  = dl_ndevs;

	if (dl_ndevs == 0)
		cdev_register(CDEV_MAJ_DL, "dl", &dl_streamtab);
	dl_ndevs++;

	struct dentry *dev = vfs_lookup("/dev");
	if (dev)
		vfs_mknod_chrdev(dev, name, MKDEV(CDEV_MAJ_DL, d->minor));
	kprintf("dl: /dev/%s raw datalink registered\n", name);
	return d;
}

void dl_input(struct dlif *d, const void *frame, unsigned len)
{
	if (!d || len < 14 || len > DL_FRAME_MAX) return;
	const uint8_t *f = frame;
	uint32_t ethertype = ((uint32_t)f[12] << 8) | f[13];

	/* Deliver UNDER dl_lock: dl_qclose takes the same lock before
	 * the stream head tears the queue pair down, so a concurrent
	 * close on another CPU can't free the queues between our
	 * used-check and the putnext (use-after-free otherwise).
	 * Lock order: dl_lock -> kmem (allocb) -> sd_readwait
	 * (sh_rq_putp); nothing takes dl_lock while holding either. */
	unsigned long fl = spin_lock_irq_save(&dl_lock);
	for (int i = 0; i < DL_MAX_OPENS; i++) {
		struct dl_open *o = &d->opens[i];
		if (!o->used || !o->drv_rq
		    || (o->sap != 0 && o->sap != ethertype))
			continue;
		mblk_t *mp = allocb(len, 0);
		if (!mp) break;
		kmemcpy(mp->b_wptr, frame, len);
		mp->b_wptr += len;
		mp->b_datap->db_type = M_DATA;
		if (o->drv_rq->q_next)
			putnext(o->drv_rq, mp);
		else
			freemsg(mp);
	}
	spin_unlock_irq_restore(&dl_lock, fl);
}

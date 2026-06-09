/*
 * kernel/stream_head.c -- STREAMS stream head, modules, drivers
 * =============================================================
 *
 * What's in here
 * --------------
 *   * Stream-head qinit (sh_rinit / sh_winit): the top of every
 *     stream stack.  sh_rq_putp queues messages arriving from below
 *     so sys_read can pop them; sh_wq_putp forwards downstream.
 *
 *   * stream_fops: the single struct file_ops that every STREAMS
 *     chrdev inode hangs its VFS dispatch off.  stream_open uses
 *     the inode's dev_t to look up cdevsw[MAJOR(rdev)], finds the
 *     driver's streamtab there, and builds a head + driver queue
 *     stack -- pure SVR4 cdevsw, not Linux's "f_ops on every inode."
 *     The VFS (kernel/vfs.c) owns the fd table; this file owns the
 *     per-stream state under file->f_private.
 *
 *   * streams_head_init: at boot, register the demo modules/drivers
 *     and publish the drivers as character-special files under /dev.
 *
 *   * Three demo modules/drivers:
 *
 *       "loop"   driver:  wq.putp echoes each mblk back up via
 *                          putnext(OTHERQ(q), mp).  Bottom of stack.
 *       "upper"  module:  rq.putp uppercases data on its way up.
 *       "delay"  module:  putp queues + qenables; srvp drains.
 *
 * Layering picture
 * ----------------
 *
 *     user --syscall--> VFS dispatch (vfs.c)
 *                           |
 *                           v
 *                       stream_fops (this file)
 *                           |
 *                           v
 *                       struct stdata + queue stack
 *                           |
 *                           v
 *                       driver (loop / future PL011 / ...)
 *
 * Pointer rewiring for I_PUSH / I_POP -- see do_ipush / do_ipop at
 * the bottom of this file.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/cdevsw.h"
#include "kappara/fbcon.h"
#include "kappara/klog.h"
#include "kappara/kmem.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/signal.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"
#include "kappara/termios.h"
#include "kappara/tty.h"
#include "kappara/uaccess.h"
#include "kappara/string.h"
#include "kappara/uart.h"
#include "kappara/vfs.h"

/* Forward-declared in streams.h. */
extern void streams_init(void);

/* ---- Module / driver registry ----------------------------------------- */

struct stmod_entry {
	const char		*name;
	struct streamtab	*st;
	struct stmod_entry	*next;
};

static struct stmod_entry *registry;

/* Head of the singly linked list of every live stdata.  stream_build
 * and pipe_end push here; stream_close removes.  /proc/streams walks
 * it to show what's currently open. */
static struct stdata *all_open_streams;

void streams_register(const char *name, struct streamtab *st)
{
	struct stmod_entry *e = kmalloc(sizeof(*e));
	if (!e) {
		kprintf("streams_register: kmalloc failed for '%s'\n", name);
		return;
	}
	e->name = name;
	e->st   = st;
	e->next = registry;
	registry = e;
}

struct streamtab *streams_lookup(const char *name)
{
	for (struct stmod_entry *e = registry; e; e = e->next)
		if (kstrcmp(e->name, name) == 0)
			return e->st;
	return NULL;
}

void streams_for_each(void (*cb)(const char *name, struct streamtab *st,
				 void *arg),
		      void *arg)
{
	for (struct stmod_entry *e = registry; e; e = e->next)
		cb(e->name, e->st, arg);
}

void streams_for_each_open(void (*cb)(struct stdata *sd, void *arg),
			   void *arg)
{
	for (struct stdata *sd = all_open_streams; sd; sd = sd->sd_all_next)
		cb(sd, arg);
}

/* ---- Stream-head queue procedures ------------------------------------- */

static int sh_rq_putp(queue_t *q, mblk_t *mp)
{
	struct stdata *sd = q->q_ptr;
	/* M_HANGUP from below means "no more data is coming" -- procfs
	 * and klog send it after their one-shot snapshot is queued.
	 * Set SD_EOF so the next sys_read returns 0 and any
	 * blocked reader wakes to see EOF.  This is the SVR4 stream
	 * head's job: M_HANGUP doesn't get queued for the reader, it
	 * mutates head state.
	 *
	 * Phase 7: SD_EOF mutation and the data putq below both happen
	 * under sd_readwait.sq_lock, the same lock stream_read holds
	 * across its (getq + EOF check + sleep_on) window.  Closes the
	 * lost-wakeup race where a reader could getq() == NULL, observe
	 * SD_EOF unset, then sleep AFTER a writer's putq + wake-all
	 * completed (with no waiters present yet). */
	/* M_IOCACK / M_IOCNAK -- response to an ioctl we started
	 * earlier via strioctl.  Stash the mblk on the stdata and wake
	 * the waiter; strioctl owns it from there.  Don't touch the
	 * data queue or sd_readwait. */
	if (sd && (mp->b_datap->db_type == M_IOCACK
	        || mp->b_datap->db_type == M_IOCNAK)) {
		unsigned long f = spin_lock_irq_save(&sd->sd_ioc_wq.sq_lock);
		if (sd->sd_ioc_response) {
			/* Spurious or duplicated ack -- drop the
			 * newer one rather than overwriting an
			 * unhandled response. */
			spin_unlock_irq_restore(&sd->sd_ioc_wq.sq_lock, f);
			freemsg(mp);
			return 0;
		}
		sd->sd_ioc_response = mp;
		spin_unlock_irq_restore(&sd->sd_ioc_wq.sq_lock, f);
		kthread_wake_all(&sd->sd_ioc_wq);
		return 0;
	}

	if (mp->b_datap->db_type == M_HANGUP) {
		freemsg(mp);
		if (sd) {
			unsigned long f =
				spin_lock_irq_save(&sd->sd_readwait.sq_lock);
			sd->sd_flags |= SD_EOF;
			spin_unlock_irq_restore(&sd->sd_readwait.sq_lock, f);
			kthread_wake_all(&sd->sd_readwait);
		}
		return 0;
	}
	if (sd) {
		unsigned long f =
			spin_lock_irq_save(&sd->sd_readwait.sq_lock);
		putq(q, mp);
		spin_unlock_irq_restore(&sd->sd_readwait.sq_lock, f);
	} else {
		putq(q, mp);
	}
	/* Wake any reader parked on this stream head's read queue.
	 * q_ptr was wired to the owning stdata in stream_build so we
	 * can hop back from the queue to its waitq without a global
	 * lookup.  The wake-all takes sd_readwait.sq_lock internally;
	 * because we already released it above, the reader either
	 * sees our putq via its own subsequent sq_lock+getq, or it
	 * was already asleep and gets surgically removed by wake_all. */
	if (sd)
		kthread_wake_all(&sd->sd_readwait);
	return 0;
}

static int sh_wq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static struct module_info sh_minfo = {
	.mi_idnum  = 0,
	.mi_idname = "strhead",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit sh_rinit = {
	.qi_putp = sh_rq_putp, .qi_minfo = &sh_minfo
};
static struct qinit sh_winit = {
	.qi_putp = sh_wq_putp, .qi_minfo = &sh_minfo
};

/* ---- The "console" driver -- real PL011 TX via STREAMS ---------------
 *
 * Bottom of stack; wq.putp walks the mblk chain and pushes every byte
 * out via uart_putc.  Bytes you `write` to /dev/console end up on the
 * serial port for real -- not echoed by ksh, not loopback'd.
 *
 * RX still goes via uart_getc_nonblock in ksh; threading the receive
 * path through STREAMS too needs a uart-rx kthread (or PL011 RX IRQ)
 * that allocb's mblks and putnext's them up.  Future commit.
 */

static int console_wq_putp(queue_t *q, mblk_t *mp)
{
	(void)q;
	/* Hold the UART lock for the whole mblk so a write() shows up
	 * as one uninterrupted run of bytes -- otherwise per-character
	 * uart_putc calls would interleave with kprintf output on other
	 * CPUs.  Shares the same lock with kprintf for ordering across
	 * both paths. */
	unsigned long f = uart_acquire();
	for (mblk_t *m = mp; m; m = m->b_cont) {
		for (unsigned char *p = m->b_rptr; p < m->b_wptr; p++) {
			uart_putc_unlocked((char)*p);
			fbcon_putc_tee((char)*p);
		}
	}
	fbcon_tee_flush();
	uart_release(f);
	freemsg(mp);
	return 0;
}

static int console_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static struct module_info console_minfo = {
	.mi_idnum  = 200,
	.mi_idname = "console",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit console_rinit = {
	.qi_putp = console_rq_putp, .qi_minfo = &console_minfo
};
static struct qinit console_winit = {
	.qi_putp = console_wq_putp, .qi_minfo = &console_minfo
};

static struct streamtab console_streamtab = {
	.st_rdinit = &console_rinit,
	.st_wrinit = &console_winit,
};

/*
 * uart_rx_main: PL011 RX -> /dev/console feeder
 * ---------------------------------------------
 * Without this, RX bytes only reach the kernel via direct
 * uart_getc_nonblock calls (what ksh used to do).  This kthread
 * threads them through STREAMS instead so sys_read on /dev/console
 * returns them and any module pushed on the stream sees them on
 * their way up.
 *
 * console_active is the first opened /dev/console stream; the
 * feeder targets it.  Subsequent opens become write-only -- they
 * can still putnext bytes down to uart_putc but get no RX, since
 * we don't fan out yet.
 */

static struct stdata *console_active;

/* Walk down the wq chain to the driver's wq, return its OTHERQ
 * (the driver's rq).  That's the bottom of the upstream chain
 * for pushing received bytes into. */
static queue_t *bottom_driver_rq(struct stdata *sd)
{
	queue_t *wq = sd->sd_wq;
	while (wq->q_next)
		wq = wq->q_next;
	return wq->q_link;
}

void uart_rx_main(void *arg)
{
	(void)arg;

	/*
	 * Phase 9 virtual-console switch keystroke: Ctrl-X N.  See
	 * the earlier note for why Ctrl-X rather than Ctrl-A (QEMU
	 * mon:stdio eats Ctrl-A).
	 */
	int vc_prefix_pending = 0;

	for (;;) {
		/* Wait until at least one possible sink exists -- either a
		 * tty open we can route to via tty_drv_rq(active) or the
		 * legacy console_active.  Draining FIFO with nowhere to
		 * put bytes would lose them. */
		if (!tty_drv_rq(tty_active()) && !console_active) {
			kthread_yield();
			continue;
		}

		int c = uart_getc_nonblock();
		if (c < 0) {
			kthread_yield();
			continue;
		}

		if (vc_prefix_pending) {
			vc_prefix_pending = 0;
			if (c >= '0' && c < '0' + NTTY) {
				tty_switch(c - '0');
				kthread_yield();
				continue;
			}
			/* Not a digit -- silently drop both the held
			 * Ctrl-A and the trailing byte.  The user typed
			 * a stale prefix; losing one byte beats injecting
			 * a Ctrl-A into the data stream. */
			kthread_yield();
			continue;
		}

		if (c == 0x18) {	/* Ctrl-X */
			vc_prefix_pending = 1;
			kthread_yield();
			continue;
		}

		/* Phase 6: prefer the currently-active tty's drv_rq;
		 * fall back to console_active if no /dev/tty<N> is
		 * open yet (early boot or pre-multi-tty user space). */
		queue_t *drq = tty_drv_rq(tty_active());
		if (!drq && console_active)
			drq = bottom_driver_rq(console_active);

		if (c == 0x03) {
			/* Ctrl-C delivery: route via the tty's fg_pgrp
			 * (phase 5) when we're on a /dev/tty<N>; fall
			 * back to console_active's sd_last_reader for
			 * the legacy path. */
			if (tty_drv_rq(tty_active())) {
				tty_signal_fg_pgrp(tty_active(), SIGINT);
			} else if (console_active) {
				struct stdata *sd = console_active;
				struct kthread *t = sd->sd_readwait.head;
				if (!t && sd->sd_last_reader)
					t = kthread_find(sd->sd_last_reader);
				if (t) kthread_signal(t, SIGINT);
			}
		} else if (drq) {
			mblk_t *mp = allocb(1, 0);
			if (mp) {
				*mp->b_wptr++ = (unsigned char)c;
				if (drq->q_next)
					putnext(drq, mp);
				else
					freemsg(mp);
			}
		}

		/* Yield every iteration so the reader thread gets a turn
		 * even when piped input is arriving faster than the reader
		 * can drain it. */
		kthread_yield();
	}
}

/* ---- The "null" driver (writes go nowhere; reads return 0 bytes) ----- */

static int null_wq_putp(queue_t *q, mblk_t *mp)
{
	(void)q;
	freemsg(mp);
	return 0;
}

static int null_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static struct module_info null_minfo = {
	.mi_idnum  = 103,
	.mi_idname = "null",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit null_rinit = {
	.qi_putp = null_rq_putp, .qi_minfo = &null_minfo
};
static struct qinit null_winit = {
	.qi_putp = null_wq_putp, .qi_minfo = &null_minfo
};

static struct streamtab null_streamtab = {
	.st_rdinit = &null_rinit,
	.st_wrinit = &null_winit,
};

/* ---- The "loop" driver ------------------------------------------------ */

static int loop_wq_putp(queue_t *q, mblk_t *mp)
{
	queue_t *up = OTHERQ(q);
	if (!up || !up->q_next) {
		freemsg(mp);
		return -1;
	}
	return putnext(up, mp);
}

static int loop_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static struct module_info loop_minfo = {
	.mi_idnum  = 100,
	.mi_idname = "loop",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit loop_rinit = {
	.qi_putp = loop_rq_putp, .qi_minfo = &loop_minfo
};
static struct qinit loop_winit = {
	.qi_putp = loop_wq_putp, .qi_minfo = &loop_minfo
};

static struct streamtab loop_streamtab = {
	.st_rdinit = &loop_rinit,
	.st_wrinit = &loop_winit,
};

/* ---- The "delay" module ----------------------------------------------- */

static int delay_putp(queue_t *q, mblk_t *mp)
{
	putq(q, mp);
	qenable(q);
	return 0;
}

static int delay_srvp(queue_t *q)
{
	mblk_t *mp;
	while ((mp = getq(q)) != NULL)
		putnext(q, mp);
	return 0;
}

static struct module_info delay_minfo = {
	.mi_idnum  = 102,
	.mi_idname = "delay",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit delay_rinit = {
	.qi_putp = delay_putp, .qi_srvp = delay_srvp, .qi_minfo = &delay_minfo
};
static struct qinit delay_winit = {
	.qi_putp = delay_putp, .qi_srvp = delay_srvp, .qi_minfo = &delay_minfo
};

static struct streamtab delay_streamtab = {
	.st_rdinit = &delay_rinit,
	.st_wrinit = &delay_winit,
};

/* ---- The "upper" module ----------------------------------------------- */

/*
 * Uppercases data in both directions.  In real SVR4 put procedures are
 * usually direction-specific (e.g. ldterm cooks input but lets output
 * through), but for a demo it's friendlier if "push upper" affects
 * everything you write/read.
 */
static void uppercase_mblk_chain(mblk_t *mp)
{
	for (mblk_t *m = mp; m; m = m->b_cont) {
		for (unsigned char *p = m->b_rptr; p < m->b_wptr; p++) {
			if (*p >= 'a' && *p <= 'z')
				*p = (unsigned char)(*p - 32);
		}
	}
}

static int upper_rq_putp(queue_t *q, mblk_t *mp)
{
	uppercase_mblk_chain(mp);
	return putnext(q, mp);
}

static int upper_wq_putp(queue_t *q, mblk_t *mp)
{
	uppercase_mblk_chain(mp);
	return putnext(q, mp);
}

static struct module_info upper_minfo = {
	.mi_idnum  = 101,
	.mi_idname = "upper",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit upper_rinit = {
	.qi_putp = upper_rq_putp, .qi_minfo = &upper_minfo
};
static struct qinit upper_winit = {
	.qi_putp = upper_wq_putp, .qi_minfo = &upper_minfo
};

static struct streamtab upper_streamtab = {
	.st_rdinit = &upper_rinit,
	.st_wrinit = &upper_winit,
};

/* ---- I_PUSH / I_POP pointer-splice helpers ---------------------------- */

static int do_ipush(struct stdata *sd, const char *modname)
{
	struct streamtab *st = streams_lookup(modname);
	if (!st)
		return -1;

	queue_t *m_rq = kmalloc(sizeof(*m_rq));
	queue_t *m_wq = kmalloc(sizeof(*m_wq));
	if (!m_rq || !m_wq)
		return -1;
	queue_init_pair(m_rq, m_wq, st->st_rdinit, st->st_wrinit);

	queue_t *old_top_wq = sd->sd_wq->q_next;
	queue_t *old_top_rq = old_top_wq->q_link;

	m_wq->q_next       = old_top_wq;
	sd->sd_wq->q_next  = m_wq;
	old_top_rq->q_next = m_rq;
	m_rq->q_next       = sd->sd_rq;

	/* SVR4: call the module's read-side qi_qopen at push time so
	 * per-instance state (ldterm's struct ldterm + termios, etc.)
	 * gets allocated.  Phase 4: previously skipped, which meant
	 * a user-space I_PUSH of any stateful module left q_ptr NULL
	 * and the first putp dereferenced through it.  Same shape
	 * stream_build uses on the driver. */
	if (st->st_rdinit && st->st_rdinit->qi_qopen)
		st->st_rdinit->qi_qopen(m_rq);
	return 0;
}

static int do_ipop(struct stdata *sd)
{
	queue_t *m_wq = sd->sd_wq->q_next;
	if (!m_wq || !m_wq->q_next)
		return -1;

	queue_t *m_rq    = m_wq->q_link;
	queue_t *next_wq = m_wq->q_next;
	queue_t *next_rq = next_wq->q_link;

	sd->sd_wq->q_next = next_wq;
	next_rq->q_next   = sd->sd_rq;

	kfree(m_wq);
	kfree(m_rq);
	return 0;
}

/* ---- stream_fops -- the file_ops the VFS dispatches through ----------- */

static struct stdata *stream_build(struct streamtab *drv_st, const char *name,
				   unsigned minor)
{
	struct stdata *sd = kmalloc(sizeof(*sd));
	queue_t *head_rq = kmalloc(sizeof(*head_rq));
	queue_t *head_wq = kmalloc(sizeof(*head_wq));
	queue_t *drv_rq  = kmalloc(sizeof(*drv_rq));
	queue_t *drv_wq  = kmalloc(sizeof(*drv_wq));
	if (!sd || !head_rq || !head_wq || !drv_rq || !drv_wq)
		return NULL;

	queue_init_pair(head_rq, head_wq, &sh_rinit,        &sh_winit);
	queue_init_pair(drv_rq,  drv_wq,  drv_st->st_rdinit, drv_st->st_wrinit);

	head_wq->q_next = drv_wq;	/* writes go down: head -> drv */
	drv_rq->q_next  = head_rq;	/* reads go up:    drv -> head */

	sd->sd_rq     = head_rq;
	sd->sd_wq     = head_wq;
	sd->sd_drv_rq = drv_rq;
	sd->sd_drv_wq = drv_wq;
	sd->sd_refs   = 1;
	sd->sd_name   = name;
	sd->sd_flags  = 0;
	sd->sd_peer   = NULL;
	sd->sd_minor  = minor;
	sd->sd_readwait = (struct wait_queue)WAIT_QUEUE_INIT;
	sd->sd_ioc_wq   = (struct wait_queue)WAIT_QUEUE_INIT;
	sd->sd_ioc_response = NULL;

	/* Backref so sh_rq_putp can wake readers without a global
	 * queue->stdata lookup.  The driver-side queues are not seen
	 * by readers, so only the head queues need the wiring. */
	head_rq->q_ptr = sd;
	head_wq->q_ptr = sd;

	/* First open of /dev/console captures the RX feed.  Subsequent
	 * opens become write-only (uart_rx_main only feeds one stream). */
	if (drv_st == &console_streamtab && !console_active)
		console_active = sd;

	/* Link onto the global open-streams list so /proc/streams can
	 * see this instance.  Singly linked, head-insert; removal is
	 * O(N) but the list is short (handful at most). */
	sd->sd_all_next = all_open_streams;
	all_open_streams = sd;

	/* Hand the stdata backref to the driver's qi_qopen via the
	 * read-queue's q_ptr.  Phase 3 multi-minor drivers (tty) use
	 * this to look up sd_minor; the qopen typically overwrites
	 * q_ptr with its own per-instance state pointer right after,
	 * so single-minor drivers that don't read q_ptr (klog, proc-*)
	 * are unaffected.  Both read AND write driver queues get the
	 * backref so any qopen-time wiring is unambiguous. */
	drv_rq->q_ptr = sd;
	drv_wq->q_ptr = sd;

	/* SVR4 streams call the driver's qi_qopen on the read side at
	 * open time.  klog uses this to prime the queue with the ring
	 * buffer contents; most drivers leave it NULL. */
	if (drv_st->st_rdinit && drv_st->st_rdinit->qi_qopen)
		drv_st->st_rdinit->qi_qopen(drv_rq);

	return sd;
}

static int stream_open(struct file *f)
{
	/* SVR4 cdev open: dev_t -> major -> cdevsw[] -> streamtab. */
	struct cdev_entry *cdev = cdev_lookup(MAJOR(f->f_inode->i_rdev));
	if (!cdev || !cdev->streamtab) {
		kprintf("stream_open: no driver at major %u\n",
			MAJOR(f->f_inode->i_rdev));
		return -1;
	}

	/* Multi-minor tty driver: multiple opens of the same /dev/ttyN
	 * share ONE stdata so all readers of the tty see the same
	 * stream-head queue and uart_rx_main only has to know about
	 * one sink per minor.  Bump sd_refs on every subsequent open;
	 * stream_close decrements and tears down on the last close. */
	if (MAJOR(f->f_inode->i_rdev) == CDEV_MAJ_TTY) {
		struct queue *drq = tty_drv_rq((int)MINOR(f->f_inode->i_rdev));
		if (drq) {
			struct stdata *existing = drq->q_ptr;
			/* drv_rq's q_ptr is &tty_minor[minor] after the
			 * driver's qopen; the tty_minor->sd backref is
			 * the actual stdata we want to share. */
			if (existing) {
				/* tty_drv_rq already returns NULL when sd
				 * is NULL, so existing is the tty_minor;
				 * fetch sd through it. */
				struct stdata *sd =
				    ((struct tty_minor *)existing)->sd;
				if (sd) {
					sd->sd_refs++;
					f->f_private = sd;
					return 0;
				}
			}
		}
	}

	struct stdata *sd = stream_build(cdev->streamtab, cdev->name,
					 MINOR(f->f_inode->i_rdev));
	if (!sd)
		return -1;

	/* Auto-push the SVR4 line discipline onto every tty open.  In
	 * Solaris this is what the autopush database (sad(7D)) drives
	 * per-device; phase 4 hardcodes "ldterm on tty" because tty
	 * is the only multi-cooking driver we have.  The push happens
	 * AFTER stream_build so the driver's qi_qopen has already
	 * wired its per-minor state via q_ptr -- ldterm pushes above
	 * it without disturbing that. */
	if (MAJOR(f->f_inode->i_rdev) == CDEV_MAJ_TTY)
		(void)do_ipush(sd, "ldterm");

	f->f_private = sd;
	return 0;
}

/* Drain every mblk queued on `q` and free them.  Safe on NULL. */
static void drain_q(queue_t *q)
{
	if (!q) return;
	mblk_t *mp;
	while ((mp = getq(q)) != NULL)
		freemsg(mp);
}

static int stream_close(struct file *f)
{
	struct stdata *sd = f->f_private;
	if (!sd) return 0;

	/* If this is one end of a pipe, the peer's reader sees EOF
	 * now that no more writers can reach it.  Wake any reader
	 * parked in stream_read so they observe SD_EOF and return 0
	 * instead of sleeping forever.  Both sides null their sd_peer
	 * so the peer's later close doesn't chase a freed pointer. */
	if (sd->sd_peer) {
		struct stdata *peer = sd->sd_peer;
		/* Same Phase-7 sq_lock interlock as sh_rq_putp's M_HANGUP
		 * branch: peer's reader checks sd_flags & SD_EOF under
		 * sd_readwait.sq_lock, so set the bit under the same lock
		 * so the reader can't miss the EOF + sleep forever. */
		unsigned long f =
			spin_lock_irq_save(&peer->sd_readwait.sq_lock);
		peer->sd_flags |= SD_EOF;
		spin_unlock_irq_restore(&peer->sd_readwait.sq_lock, f);
		peer->sd_peer   = NULL;
		sd->sd_peer     = NULL;
		kthread_wake_all(&peer->sd_readwait);
	}

	f->f_private = NULL;
	if (--sd->sd_refs > 0)
		return 0;

	/* Don't leave uart_rx pointing at a stream we're about to
	 * free.  In practice init never closes /dev/console, so this
	 * branch is defensive. */
	if (console_active == sd)
		console_active = NULL;

	/* Last reference: free the queue stack + stdata.  Order:
	 *   1. drain any queued mblks so the buffers go back to slab
	 *   2. walk head_wq down via q_next freeing each module pair
	 *      until we hit the driver pair (recorded separately)
	 *      or NULL (pipe case: the chain points at the peer, not
	 *      our own queues, so we stop at the head pair)
	 *   3. free the driver pair (NULL for pipes)
	 *   4. free stdata itself
	 *
	 * Pipe special-case: sd_wq->q_next was set to the peer's rq
	 * for cross-wiring; we must NOT walk past our own head pair
	 * in that case.  Detect by sd_drv_wq == NULL. */
	drain_q(sd->sd_rq);
	drain_q(sd->sd_wq);

	if (sd->sd_drv_wq) {
		/* Walk the pushed-module pairs.  For each pair: invoke its
		 * qi_qclose (so the module can free per-instance state +
		 * clear any backrefs from upper modules to its q_ptr),
		 * then drain + free both queues. */
		queue_t *q = sd->sd_wq->q_next;
		while (q && q != sd->sd_drv_wq) {
			queue_t *next = q->q_next;
			queue_t *peer_q = q->q_link;
			if (q->q_qinfo && q->q_qinfo->qi_qclose)
				q->q_qinfo->qi_qclose(peer_q);
			drain_q(q);
			drain_q(peer_q);
			kfree(q);
			kfree(peer_q);
			q = next;
		}
		/* Now the driver pair.  qi_qclose on the read side gives
		 * multi-minor drivers (tty) a chance to null out the
		 * per-minor sd backref so the next open of the same minor
		 * doesn't latch onto a freed stdata. */
		if (sd->sd_drv_rq->q_qinfo
		    && sd->sd_drv_rq->q_qinfo->qi_qclose)
			sd->sd_drv_rq->q_qinfo->qi_qclose(sd->sd_drv_rq);
		drain_q(sd->sd_drv_rq);
		drain_q(sd->sd_drv_wq);
		kfree(sd->sd_drv_rq);
		kfree(sd->sd_drv_wq);
	}

	/* Splice out of the global open-streams list before freeing
	 * so /proc/streams can never dereference a stale pointer. */
	struct stdata **link = &all_open_streams;
	while (*link && *link != sd) link = &(*link)->sd_all_next;
	if (*link == sd) *link = sd->sd_all_next;

	kfree(sd->sd_rq);
	kfree(sd->sd_wq);
	kfree(sd);
	return 0;
}

/*
 * SVR4 read vs getmsg
 * -------------------
 * Both consume from the stream head's read queue, but they have
 * different semantics on purpose:
 *
 *   read   -- byte-stream view.  Splits an mblk mid-buffer if `len`
 *             is smaller than the mblk, putbq()s the remainder, and
 *             chains across multiple b_cont mblks to fill `len`.
 *             Message boundaries are invisible to the caller.
 *
 *   getmsg -- message-oriented view.  Returns one whole logical
 *             message (the b_cont chain rooted at one getq) and
 *             splits its ctl (M_PROTO) and data (M_DATA) pieces
 *             into separate strbufs so the caller sees protocol
 *             metadata next to the payload it accompanies.
 *
 * read is therefore NOT a thin wrapper over getmsg -- they really
 * are separate read paths.  write, on the other hand, IS just a
 * putmsg with ctl=NULL/data=buf -- see stream_write below.
 */
static long stream_read(struct file *f, void *buf, size_t len)
{
	struct stdata *sd = f->f_private;
	if (!sd)
		return -1;

	/* Record this thread as the latest reader so the console's
	 * Ctrl-C delivery (uart_rx_main below) can find a target even
	 * when we're between sleep-and-wake at the moment 0x03 arrives.
	 * For non-console streams this is harmless. */
	if (curthread) sd->sd_last_reader = curthread->tid;

	/* Block until something arrives on the head's read queue, or
	 * the peer closes (SD_EOF) and the backlog is drained.  The
	 * SD_EOF check after getq is what gives us proper Unix EOF
	 * semantics: drained data is still returned before the 0.
	 *
	 * Phase 7: take sd_readwait.sq_lock around the whole (getq +
	 * EOF check + sleep_on) window.  sh_rq_putp / pipe close take
	 * the same lock around their putq + SD_EOF mutations, so the
	 * check-then-sleep race that drops a writer's wake when it
	 * fires "too early" (before the reader is on the wq) is
	 * closed.  kthread_sleep_on_locked releases sq_lock via the
	 * phase-5 extra_release pathway -- the reader is on the wq
	 * before any new writer can acquire sq_lock and putq more
	 * data. */
	mblk_t *mp;
	unsigned long flags = spin_lock_irq_save(&sd->sd_readwait.sq_lock);
	for (;;) {
		mp = getq(sd->sd_rq);
		if (mp) break;
		if (sd->sd_flags & SD_EOF) {
			spin_unlock_irq_restore(&sd->sd_readwait.sq_lock, flags);
			return 0;	/* EOF -- no more writers */
		}
		/* A pending fatal signal short-circuits the read so the
		 * thread can exit cleanly in check_signals on the way
		 * back out of the syscall.  Mirrors EINTR on real Unix
		 * (we don't surface errno yet). */
		{ struct kthread *me = curthread;
		  if (me && (me->sig_pending & SIG_FATAL_MASK)) {
			spin_unlock_irq_restore(&sd->sd_readwait.sq_lock,
					        flags);
			return -1;
		  }
		}
		kthread_sleep_on_locked(&sd->sd_readwait, flags);
		/* sleep_on_locked releases sq_lock as part of ctx_switch
		 * save and restores our caller's IRQ state at the tail.
		 * Re-acquire for the next loop iteration's getq. */
		flags = spin_lock_irq_save(&sd->sd_readwait.sq_lock);
		{ struct kthread *me = curthread;
		  if (me && (me->sig_pending & SIG_FATAL_MASK)) {
			spin_unlock_irq_restore(&sd->sd_readwait.sq_lock,
					        flags);
			return -1;
		  }
		}
	}
	spin_unlock_irq_restore(&sd->sd_readwait.sq_lock, flags);

	/*
	 * Walk the b_cont chain copying out bytes, advancing each mblk's
	 * b_rptr as we go.  When we run out of room (copied == len):
	 *   - mp has leftover -> putbq(mp) so the next read gets it
	 *   - mp's b_cont has leftover -> disconnect the drained prefix,
	 *     freemsg it, putbq the tail
	 * Otherwise (we drained the whole chain) just freemsg(mp).
	 *
	 * `cur` tracks the mblk we're currently consuming.  After the
	 * loop, cur != NULL means there's leftover starting at cur.
	 */
	unsigned char *out = buf;
	size_t copied = 0;
	mblk_t *cur = mp;
	while (cur && copied < len) {
		size_t avail = (size_t)(cur->b_wptr - cur->b_rptr);
		size_t room  = len - copied;
		size_t n     = (avail < room) ? avail : room;
		kmemcpy(out + copied, cur->b_rptr, n);
		copied      += n;
		cur->b_rptr += n;
		if (cur->b_rptr == cur->b_wptr) {
			cur = cur->b_cont;	/* fully drained; advance */
		} else {
			break;			/* leftover within this mblk */
		}
	}

	/* putbq mutates sd_rq -- same queue sh_rq_putp's putq writes
	 * to.  Take sd_readwait.sq_lock for the leftover put-back so a
	 * concurrent putq/getq doesn't trip over a half-updated
	 * q_first/q_last pair.  Lock is released immediately; the
	 * caller already copied data to user above without holding it,
	 * which is what we want (no spinlock across uaccess). */
	if (cur && cur != mp) {
		/* Drained mp..(predecessor of cur); leftover starts at cur. */
		mblk_t *prev = mp;
		while (prev->b_cont != cur)
			prev = prev->b_cont;
		prev->b_cont = NULL;
		freemsg(mp);
		unsigned long f2 =
			spin_lock_irq_save(&sd->sd_readwait.sq_lock);
		putbq(sd->sd_rq, cur);
		spin_unlock_irq_restore(&sd->sd_readwait.sq_lock, f2);
	} else if (cur) {
		/* mp itself has leftover; put it back unchanged. */
		unsigned long f2 =
			spin_lock_irq_save(&sd->sd_readwait.sq_lock);
		putbq(sd->sd_rq, mp);
		spin_unlock_irq_restore(&sd->sd_readwait.sq_lock, f2);
	} else {
		/* Chain fully drained. */
		freemsg(mp);
	}
	return (long)copied;
}

static long stream_putmsg(struct file *f, const struct strbuf *ctl,
			  const struct strbuf *data, int flags);

/*
 * SVR4 write is a documented special case of putmsg: ctl=NULL,
 * data={buf,len}, flags=0.  Same M_DATA mblk lands on the same
 * downstream queue.  We make the equivalence literal here so any
 * change to message-build semantics happens in one place.
 *
 * The single deviation: write() returns the byte count on success
 * (POSIX), putmsg() returns 0 (SVR4).  We translate on the boundary.
 */
static long stream_write(struct file *f, const void *buf, size_t len)
{
	struct strbuf data = {
		.maxlen = 0,
		.len    = (int)len,
		.buf    = (void *)(uintptr_t)buf,
	};
	long r = stream_putmsg(f, NULL, &data, 0);
	return r < 0 ? r : (long)len;
}

/* ---- M_IOCTL strioctl glue --------------------------------------------- */

/* Per-cmd payload size table -- maps the ioctl command to the
 * number of bytes the user buffer holds.  Used by both the
 * "build M_IOCTL going down" and the "copy response back to user"
 * paths so the two stay symmetric.  Unknown commands get 0 and
 * stream_ioctl bails out before allocating a payload mblk. */
static int strioctl_payload_size(int cmd)
{
	switch (cmd) {
	case TCGETA:
	case TCSETA:
	case TCSETAW:
	case TCSETAF:
		return (int)sizeof(struct termios);
	case TCFLSH:
		return 0;
	default:
		return -1;
	}
}

static long strioctl(struct stdata *sd, int cmd, long arg)
{
	int payload = strioctl_payload_size(cmd);
	if (payload < 0)
		return -1;
	if (!sd->sd_wq->q_next)
		return -1;

	/* Build the M_IOCTL mblk: iocblk header in the first mblk, the
	 * payload (a struct termios for TCSETA, etc.) chained via
	 * b_cont.  TCSETA / TCFLSH copy data IN from user; TCGETA gets
	 * its payload filled by the module on the way back up. */
	mblk_t *iocmp = allocb(sizeof(struct iocblk), 0);
	if (!iocmp) return -1;
	iocmp->b_datap->db_type = M_IOCTL;
	struct iocblk *ic = (struct iocblk *)iocmp->b_wptr;
	ic->ic_cmd   = cmd;
	ic->ic_count = payload;
	ic->ic_error = 0;
	ic->ic_tid   = curthread ? (int)curthread->tid : -1;
	iocmp->b_wptr += sizeof(*ic);

	if (payload > 0) {
		mblk_t *data = allocb((size_t)payload, 0);
		if (!data) {
			freemsg(iocmp);
			return -1;
		}
		/* TCSETA / TCSETAW / TCSETAF carry data IN -- copy from
		 * user space (or kernel, for in-kernel callers).
		 * TCGETA carries data OUT only -- the module fills it
		 * on the way back. */
		if (cmd == TCSETA || cmd == TCSETAW || cmd == TCSETAF) {
			if (syscall_from_user) {
				if (copy_from_user(data->b_wptr,
				                   (const void *)(uintptr_t)arg,
				                   (size_t)payload) < 0) {
					freemsg(iocmp);
					freemsg(data);
					return -1;
				}
			} else {
				kmemcpy(data->b_wptr,
				        (const void *)(uintptr_t)arg,
				        (size_t)payload);
			}
		}
		data->b_wptr += payload;
		iocmp->b_cont = data;
	}

	/* Send the M_IOCTL down the write side.  Modules + driver get
	 * their crack at it via their qi_putp; whoever recognises
	 * ic_cmd flips db_type and putnexts back up. */
	putnext(sd->sd_wq, iocmp);

	/* Wait for sh_rq_putp to stash the response.  sd_ioc_wq is
	 * the rendezvous queue.  We hold sq_lock around the check +
	 * sleep to close the lost-wakeup window that phases 5c/6/7
	 * handled for the data path. */
	unsigned long flags = spin_lock_irq_save(&sd->sd_ioc_wq.sq_lock);
	while (sd->sd_ioc_response == NULL) {
		kthread_sleep_on_locked(&sd->sd_ioc_wq, flags);
		flags = spin_lock_irq_save(&sd->sd_ioc_wq.sq_lock);
	}
	mblk_t *resp = sd->sd_ioc_response;
	sd->sd_ioc_response = NULL;
	spin_unlock_irq_restore(&sd->sd_ioc_wq.sq_lock, flags);

	/* Decode the response.  iocblk in the head mblk, payload (if
	 * any) in b_cont.  ic_error == 0 + db_type == M_IOCACK means
	 * success; M_IOCNAK or non-zero ic_error means failure. */
	long ret = -1;
	struct iocblk *ric = (struct iocblk *)resp->b_rptr;
	if (resp->b_datap->db_type == M_IOCACK && ric->ic_error == 0) {
		ret = 0;
		if (cmd == TCGETA && resp->b_cont) {
			mblk_t *rdata = resp->b_cont;
			size_t n = (size_t)(rdata->b_wptr - rdata->b_rptr);
			if (n > (size_t)payload) n = (size_t)payload;
			if (syscall_from_user) {
				if (copy_to_user((void *)(uintptr_t)arg,
				                 rdata->b_rptr, n) < 0)
					ret = -1;
			} else {
				kmemcpy((void *)(uintptr_t)arg, rdata->b_rptr, n);
			}
		}
	}
	freemsg(resp);
	return ret;
}

static long stream_ioctl(struct file *f, int cmd, long arg)
{
	struct stdata *sd = f->f_private;
	if (!sd)
		return -1;
	switch (cmd) {
	case I_PUSH:
		return do_ipush(sd, (const char *)(uintptr_t)arg);
	case I_POP:
		return do_ipop(sd);
	default:
		/* Everything else flows down the stream as M_IOCTL.  The
		 * module or driver that owns the command (TCGETA on
		 * ldterm, future driver-private commands on a tty
		 * driver, ...) processes it and responds via M_IOCACK /
		 * M_IOCNAK. */
		return strioctl(sd, cmd, arg);
	}
}

static long stream_putmsg(struct file *f, const struct strbuf *ctl,
			  const struct strbuf *data, int flags)
{
	struct stdata *sd = f->f_private;
	if (!sd || !sd->sd_wq->q_next)
		return -1;
	(void)flags;

	mblk_t *ctlmp = NULL, *datamp = NULL;

	if (ctl && ctl->len > 0 && ctl->buf) {
		ctlmp = allocb((size_t)ctl->len, 0);
		if (!ctlmp)
			return -1;
		ctlmp->b_datap->db_type = M_PROTO;
		kmemcpy(ctlmp->b_wptr, ctl->buf, (size_t)ctl->len);
		ctlmp->b_wptr += ctl->len;
	}
	if (data && data->len > 0 && data->buf) {
		datamp = allocb((size_t)data->len, 0);
		if (!datamp) {
			if (ctlmp) freemsg(ctlmp);
			return -1;
		}
		kmemcpy(datamp->b_wptr, data->buf, (size_t)data->len);
		datamp->b_wptr += data->len;
	}

	mblk_t *head = ctlmp;
	if (head)
		head->b_cont = datamp;
	else
		head = datamp;
	if (!head)
		return 0;

	putnext(sd->sd_wq, head);
	return 0;
}

static long stream_getmsg(struct file *f, struct strbuf *ctl,
			  struct strbuf *data, int *flagsp)
{
	struct stdata *sd = f->f_private;
	if (!sd)
		return -1;

	mblk_t *mp = getq(sd->sd_rq);
	if (!mp)
		return -1;

	mblk_t *cptr = NULL;
	mblk_t *dptr = NULL;
	if (mp->b_datap->db_type == M_PROTO) {
		cptr = mp;
		dptr = mp->b_cont;
	} else {
		dptr = mp;
	}

	if (ctl) {
		if (cptr) {
			size_t n = (size_t)(cptr->b_wptr - cptr->b_rptr);
			if ((int)n > ctl->maxlen) n = (size_t)ctl->maxlen;
			kmemcpy(ctl->buf, cptr->b_rptr, n);
			ctl->len = (int)n;
		} else {
			ctl->len = -1;
		}
	}
	if (data) {
		if (dptr) {
			size_t n = (size_t)(dptr->b_wptr - dptr->b_rptr);
			if ((int)n > data->maxlen) n = (size_t)data->maxlen;
			kmemcpy(data->buf, dptr->b_rptr, n);
			data->len = (int)n;
		} else {
			data->len = -1;
		}
	}
	if (flagsp)
		*flagsp = 0;

	freemsg(mp);
	return 0;
}

struct file_ops stream_fops = {
	.open   = stream_open,
	.close  = stream_close,
	.read   = stream_read,
	.write  = stream_write,
	.ioctl  = stream_ioctl,
	.putmsg = stream_putmsg,
	.getmsg = stream_getmsg,
};

/* ---- sys_pipe: anonymous STREAMS pipe -------------------------------- */

/*
 * Build two stream heads (no driver, no inode) and wire each one's
 * write side to the other's read side.  Writing to A's wq -> putnext
 * -> B's rq.putp (sh_rq_putp) -> putq.  A's reader getq's its own rq,
 * which received whatever B has written.
 *
 *     +-----+      head_wq.q_next      +-----+
 *     |  A  | -----------------------> |  B  |
 *     |head | <----------------------- |head |
 *     +-----+      head_wq.q_next      +-----+
 *           (each direction is a head-to-peer-head edge)
 */
static struct stdata *pipe_end(const char *name)
{
	struct stdata *sd = kmalloc(sizeof(*sd));
	queue_t *rq = kmalloc(sizeof(*rq));
	queue_t *wq = kmalloc(sizeof(*wq));
	if (!sd || !rq || !wq)
		return NULL;
	queue_init_pair(rq, wq, &sh_rinit, &sh_winit);
	sd->sd_rq     = rq;
	sd->sd_wq     = wq;
	sd->sd_drv_rq = NULL;	/* no driver for pipes */
	sd->sd_drv_wq = NULL;
	sd->sd_refs   = 1;
	sd->sd_name   = name;
	sd->sd_flags  = 0;
	sd->sd_peer   = NULL;
	sd->sd_minor  = 0;	/* pipes have no minor */
	sd->sd_readwait = (struct wait_queue)WAIT_QUEUE_INIT;
	sd->sd_ioc_wq   = (struct wait_queue)WAIT_QUEUE_INIT;
	sd->sd_ioc_response = NULL;
	rq->q_ptr = sd;	/* so sh_rq_putp can wake readers */
	wq->q_ptr = sd;
	sd->sd_all_next = all_open_streams;
	all_open_streams = sd;
	return sd;
}

long sys_pipe_impl(int fds[2])
{
	struct stdata *a = pipe_end("pipe-a");
	struct stdata *b = pipe_end("pipe-b");
	if (!a || !b)
		return -1;

	/* Cross-wire: writes on A land on B's rq, and vice versa. */
	a->sd_wq->q_next = b->sd_rq;
	b->sd_wq->q_next = a->sd_rq;

	/* Pipe-peer pointers: when one end closes, stream_close uses
	 * these to set SD_EOF on the OTHER end (since the closer's
	 * writes were the only thing feeding the peer's read queue). */
	a->sd_peer = b;
	b->sd_peer = a;

	struct file *fa = kmalloc(sizeof(*fa));
	struct file *fb = kmalloc(sizeof(*fb));
	if (!fa || !fb)
		return -1;
	fa->f_ops = &stream_fops;
	fa->f_inode = NULL;
	fa->f_private = a;
	fa->f_refs = 1;
	fb->f_ops = &stream_fops;
	fb->f_inode = NULL;
	fb->f_private = b;
	fb->f_refs = 1;

	int rd = fd_alloc(fa);
	int wr = fd_alloc(fb);
	if (rd < 0 || wr < 0)
		return -1;

	fds[0] = rd;	/* convention: fds[0] = read end, fds[1] = write */
	fds[1] = wr;
	return 0;
}

/* ---- The "klog" driver: replay the kernel log ring ------------------- */

/*
 * Pure SVR4 STREAMS driver: no per-file open hook.  The priming-from-
 * ring step is the read-side qi_qopen, invoked by stream_build the
 * moment the queue pair is wired up.  After that, sys_read just
 * drains the queued mblks via getq.  Write side rejects everything;
 * klog is read-only.
 */
static int klog_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static int klog_wq_putp(queue_t *q, mblk_t *mp)
{
	(void)q;
	freemsg(mp);
	return -1;	/* read-only device */
}

#define KLOG_CHUNK	1024

/* SVR4 read-side qopen: prime the stream with the current klog ring
 * by putnext'ing one mblk per KLOG_CHUNK upstream (so it lands on
 * the head's read queue, where sys_read drains from).  One allocation
 * per chunk so we stay inside the slab size caches (allocb caps at
 * the kmalloc max). */
static int klog_rq_qopen(queue_t *q)
{
	size_t total = klog_size();
	for (size_t off = 0; off < total; off += KLOG_CHUNK) {
		size_t want = total - off;
		if (want > KLOG_CHUNK) want = KLOG_CHUNK;
		mblk_t *mp = allocb(want, 0);
		if (!mp) break;
		size_t got = klog_copy((char *)mp->b_wptr, off, want);
		mp->b_wptr += got;
		putnext(q, mp);
	}
	/* Snapshot is done -- no more data will be queued, so signal
	 * EOF to any reader.  M_HANGUP travels up like any other
	 * message and the stream head turns it into SD_EOF. */
	mblk_t *hup = allocb(1, 0);
	if (hup) {
		hup->b_datap->db_type = M_HANGUP;
		putnext(q, hup);
	}
	return 0;
}

static struct module_info klog_minfo = {
	.mi_idnum  = 201,
	.mi_idname = "klog",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit klog_rinit = {
	.qi_putp   = klog_rq_putp,
	.qi_qopen  = klog_rq_qopen,
	.qi_minfo  = &klog_minfo
};
static struct qinit klog_winit = {
	.qi_putp = klog_wq_putp, .qi_minfo = &klog_minfo
};

static struct streamtab klog_streamtab = {
	.st_rdinit = &klog_rinit,
	.st_wrinit = &klog_winit,
};

/* ---- Boot-time registration ------------------------------------------ */

/* Defined in arch/aarch64/fbcon.c; not present on ARMv7 (no framebuffer yet).
 * Conditional on __aarch64__ so the ARMv7 build links cleanly. */
#ifdef __aarch64__
extern struct streamtab fbcon_streamtab;
#endif

void streams_head_init(void)
{
	/* Bring up the mblk/dblk/queue slab caches.  Must run before
	 * any allocb() call, which means before any stream is opened. */
	streams_init();

	/* The by-name registry stays -- it backs I_PUSH module lookup,
	 * which is name-keyed in SVR4.  Modules with no chrdev (upper,
	 * delay) live here only. */
	streams_register("loop",    &loop_streamtab);
	streams_register("null",    &null_streamtab);
	streams_register("console", &console_streamtab);
	streams_register("klog",    &klog_streamtab);
	streams_register("upper",   &upper_streamtab);
	streams_register("delay",   &delay_streamtab);
#ifdef __aarch64__
	streams_register("fbcon",   &fbcon_streamtab);
#endif

	/* SVR4 line discipline -- registers "ldterm" so a tty stream
	 * can do I_PUSH "ldterm" and get cooked-mode + termios. */
	ldterm_init();

	/* SVR4 cdevsw: each openable STREAMS driver claims a major
	 * number.  Modules (upper, delay) are pushed onto an already-
	 * open stream and don't need a major; we register them here
	 * anyway so /dev/upper and /dev/delay are openable too -- handy
	 * for testing the module logic in isolation. */
	cdev_register(CDEV_MAJ_LOOP,    "loop",    &loop_streamtab);
	cdev_register(CDEV_MAJ_NULL,    "null",    &null_streamtab);
	cdev_register(CDEV_MAJ_CONSOLE, "console", &console_streamtab);
	cdev_register(CDEV_MAJ_KLOG,    "klog",    &klog_streamtab);
	cdev_register(CDEV_MAJ_UPPER,   "upper",   &upper_streamtab);
	cdev_register(CDEV_MAJ_DELAY,   "delay",   &delay_streamtab);
#ifdef __aarch64__
	cdev_register(CDEV_MAJ_FBCON,   "fbcon",   &fbcon_streamtab);
#endif

	/* Publish under /dev with their dev_t.  The VFS open path
	 * looks up cdevsw[MAJOR(rdev)] to find the streamtab; the
	 * inode no longer holds the driver pointer directly. */
	struct dentry *dev = vfs_mkdir(vfs_root(), "dev");
	vfs_mknod_chrdev(dev, "loop",    MKDEV(CDEV_MAJ_LOOP,    0));
	vfs_mknod_chrdev(dev, "null",    MKDEV(CDEV_MAJ_NULL,    0));
	vfs_mknod_chrdev(dev, "console", MKDEV(CDEV_MAJ_CONSOLE, 0));
	vfs_mknod_chrdev(dev, "klog",    MKDEV(CDEV_MAJ_KLOG,    0));
#ifdef __aarch64__
	vfs_mknod_chrdev(dev, "fbcon",   MKDEV(CDEV_MAJ_FBCON,   0));
#endif

	/* Phase 3 virtual-console TTYs.  tty_init registers the cdev
	 * + streamtab; the /dev nodes go here so /dev lookup stays in
	 * one place. */
	tty_init();
	vfs_mknod_chrdev(dev, "tty0", MKDEV(CDEV_MAJ_TTY, 0));
	vfs_mknod_chrdev(dev, "tty1", MKDEV(CDEV_MAJ_TTY, 1));
	vfs_mknod_chrdev(dev, "tty2", MKDEV(CDEV_MAJ_TTY, 2));
	vfs_mknod_chrdev(dev, "tty3", MKDEV(CDEV_MAJ_TTY, 3));

	kprintf("stream_head: registered modules:");
	for (struct stmod_entry *e = registry; e; e = e->next)
		kprintf(" %s", e->name);
	kprintf("\n");
}

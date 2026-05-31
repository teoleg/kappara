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
#include "kappara/stream_head.h"
#include "kappara/streams.h"
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

/* ---- Stream-head queue procedures ------------------------------------- */

static int sh_rq_putp(queue_t *q, mblk_t *mp)
{
	putq(q, mp);
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
	for (mblk_t *m = mp; m; m = m->b_cont) {
		for (unsigned char *p = m->b_rptr; p < m->b_wptr; p++) {
			uart_putc((char)*p);
			fbcon_putc_tee((char)*p);
		}
	}
	fbcon_tee_flush();
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
	for (;;) {
		if (!console_active) {
			/* Nothing listening yet -- don't drain the FIFO
			 * or those bytes are lost.  Wait for the first
			 * open of /dev/console. */
			kthread_yield();
			continue;
		}

		int c = uart_getc_nonblock();
		if (c >= 0) {
			mblk_t *mp = allocb(1, 0);
			if (mp) {
				*mp->b_wptr++ = (unsigned char)c;
				queue_t *drq = bottom_driver_rq(console_active);
				if (drq && drq->q_next)
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

static struct stdata *stream_build(struct streamtab *drv_st, const char *name)
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

	sd->sd_rq   = head_rq;
	sd->sd_wq   = head_wq;
	sd->sd_refs = 1;
	sd->sd_name = name;

	/* First open of /dev/console captures the RX feed.  Subsequent
	 * opens become write-only (uart_rx_main only feeds one stream). */
	if (drv_st == &console_streamtab && !console_active)
		console_active = sd;

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
	struct stdata *sd = stream_build(cdev->streamtab, cdev->name);
	if (!sd)
		return -1;
	f->f_private = sd;
	return 0;
}

static int stream_close(struct file *f)
{
	/* TODO: walk and free the queue stack + drain. */
	f->f_private = NULL;
	return 0;
}

static long stream_read(struct file *f, void *buf, size_t len)
{
	struct stdata *sd = f->f_private;
	if (!sd)
		return -1;
	mblk_t *mp = getq(sd->sd_rq);
	if (!mp)
		return 0;

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

	if (cur && cur != mp) {
		/* Drained mp..(predecessor of cur); leftover starts at cur. */
		mblk_t *prev = mp;
		while (prev->b_cont != cur)
			prev = prev->b_cont;
		prev->b_cont = NULL;
		freemsg(mp);
		putbq(sd->sd_rq, cur);
	} else if (cur) {
		/* mp itself has leftover; put it back unchanged. */
		putbq(sd->sd_rq, mp);
	} else {
		/* Chain fully drained. */
		freemsg(mp);
	}
	return (long)copied;
}

static long stream_write(struct file *f, const void *buf, size_t len)
{
	struct stdata *sd = f->f_private;
	if (!sd)
		return -1;
	mblk_t *mp = allocb(len, 0);
	if (!mp)
		return -1;
	kmemcpy(mp->b_wptr, buf, len);
	mp->b_wptr += len;
	if (!sd->sd_wq->q_next) {
		freemsg(mp);
		return -1;
	}
	putnext(sd->sd_wq, mp);
	return (long)len;
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
		return -1;
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
	sd->sd_rq   = rq;
	sd->sd_wq   = wq;
	sd->sd_refs = 1;
	sd->sd_name = name;
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

	kprintf("stream_head: registered modules:");
	for (struct stmod_entry *e = registry; e; e = e->next)
		kprintf(" %s", e->name);
	kprintf("\n");
}

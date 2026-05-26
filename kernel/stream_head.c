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
 *   * stream_fops: the struct file_ops the VFS dispatches through
 *     when an open file points at a character-special inode whose
 *     i_private is a streamtab pointer.  The VFS (kernel/vfs.c)
 *     owns the fd table; this file owns the per-stream state under
 *     file->f_private.
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

#include "kappara/kmem.h"
#include "kappara/printk.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"
#include "kappara/string.h"
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

static int upper_rq_putp(queue_t *q, mblk_t *mp)
{
	for (mblk_t *m = mp; m; m = m->b_cont) {
		for (unsigned char *p = m->b_rptr; p < m->b_wptr; p++) {
			if (*p >= 'a' && *p <= 'z')
				*p = (unsigned char)(*p - 32);
		}
	}
	return putnext(q, mp);
}

static int upper_wq_putp(queue_t *q, mblk_t *mp)
{
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
	return sd;
}

static int stream_open(struct file *f)
{
	struct streamtab *st = (struct streamtab *)f->f_inode->i_private;
	if (!st)
		return -1;
	struct stdata *sd = stream_build(st, NULL);
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

	unsigned char *out = buf;
	size_t copied = 0;
	for (mblk_t *m = mp; m && copied < len; m = m->b_cont) {
		size_t avail = (size_t)(m->b_wptr - m->b_rptr);
		size_t room  = len - copied;
		size_t n     = (avail < room) ? avail : room;
		kmemcpy(out + copied, m->b_rptr, n);
		copied += n;
	}
	freemsg(mp);
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

/* ---- Boot-time registration ------------------------------------------ */

void streams_head_init(void)
{
	/* Bring up the mblk/dblk/queue slab caches.  Must run before
	 * any allocb() call, which means before any stream is opened. */
	streams_init();

	streams_register("loop",  &loop_streamtab);
	streams_register("null",  &null_streamtab);
	streams_register("upper", &upper_streamtab);
	streams_register("delay", &delay_streamtab);

	/* Publish drivers as character-special files under /dev.
	 * Modules ("upper", "delay") stay registry-only -- they
	 * are not openable, they're pushed via ioctl(I_PUSH). */
	struct dentry *dev = vfs_mkdir(vfs_root(), "dev");
	vfs_mknod_chrdev(dev, "loop", &stream_fops, &loop_streamtab);
	vfs_mknod_chrdev(dev, "null", &stream_fops, &null_streamtab);

	kprintf("stream_head: registered modules:");
	for (struct stmod_entry *e = registry; e; e = e->next)
		kprintf(" %s", e->name);
	kprintf("\n");
}

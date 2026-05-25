/*
 * kernel/stream_head.c -- STREAMS stream head + fd layer + demo drivers
 * =====================================================================
 *
 * What this file is
 * -----------------
 * The piece that makes the STREAMS subsystem useful from syscalls.
 * Three things live here:
 *
 *   1. The stream-head implementation: a queue pair (sd_rq/sd_wq)
 *      that bridges sys_read/write/ioctl to the message-passing
 *      stack underneath.
 *
 *   2. A small global fd table that sys_open populates, indexed by
 *      a small int returned to user.  No per-process fd table yet
 *      because there are no processes -- the whole kernel shares
 *      one table.
 *
 *   3. A driver and module registry so I_PUSH "upper" and
 *      sys_open("loop") can look things up by name.  Two demo
 *      modules are registered at boot:
 *
 *        "loop"   driver:  wq.putp echoes each message straight
 *                          back up via putnext(OTHERQ(q), mp).
 *                          Bottom of the stack.
 *
 *        "upper"  module:  rq.putp uppercases the data in each
 *                          mblk before forwarding upward.  Demonstrates
 *                          how I_PUSH inserts a transformer between
 *                          the user and the driver.
 *
 * Pointer rewiring for I_PUSH / I_POP
 * -----------------------------------
 *
 *   Initial state (one driver, no modules):
 *
 *     head_wq.q_next = drv_wq            head_rq.q_next = NULL  (top)
 *     drv_wq.q_next  = NULL  (bottom)    drv_rq.q_next  = head_rq
 *
 *   After I_PUSH "M" (insert M between head and current top-below):
 *
 *     head_wq.q_next = M_wq               head_rq.q_next = NULL
 *     M_wq.q_next    = old_top_wq         M_rq.q_next    = head_rq
 *     old_top_wq...  unchanged            old_top_rq.q_next = M_rq
 *
 *   So three pointers move on each side: insert M, splice the chain.
 *
 *   I_POP just undoes that: pull M out, restore the four pointers.
 *
 * Memory ownership
 * ----------------
 * Each queue_t is kmalloc'd individually.  qinit / module_info are
 * static (per-module compile-time constants).  stdata is kmalloc'd
 * per-open.  sys_close currently leaks the queue chain -- TODO when
 * we have I_PLINK semantics.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/kmem.h"
#include "kappara/printk.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"
#include "kappara/string.h"

/* ---- fd table ----------------------------------------------------------- */

#define FD_MAX	16

struct file {
	struct stdata	*f_stream;
	int		 f_used;
};

static struct file fd_table[FD_MAX];

/* ---- Module / driver registry ------------------------------------------ */

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

/* ---- Stream-head queue procedures -------------------------------------- */

/* The head's rq.putp is what messages travelling up from below
 * land in.  We just queue them so sys_read can pull them off. */
static int sh_rq_putp(queue_t *q, mblk_t *mp)
{
	putq(q, mp);
	return 0;
}

/* The head's wq.putp is what sys_write puts to.  Forward down. */
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

/* ---- The "loop" driver ------------------------------------------------- */

/* Driver write side: bottom of the stack.  Echo upward via OTHERQ. */
static int loop_wq_putp(queue_t *q, mblk_t *mp)
{
	queue_t *up = OTHERQ(q);
	if (!up || !up->q_next) {
		freemsg(mp);
		return -1;
	}
	return putnext(up, mp);
}

/* Driver read side: not normally called from above (we ARE the bottom);
 * forward upward if we ever are. */
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

/* ---- The "upper" module ----------------------------------------------- */

/* Read side: uppercases data on its way up, then forwards. */
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

/* Write side: pass-through. */
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

/* ---- Init ------------------------------------------------------------- */

void streams_head_init(void)
{
	streams_register("loop",  &loop_streamtab);
	streams_register("upper", &upper_streamtab);

	for (int i = 0; i < FD_MAX; i++) {
		fd_table[i].f_used   = 0;
		fd_table[i].f_stream = NULL;
	}

	kprintf("stream_head: registered modules:");
	for (struct stmod_entry *e = registry; e; e = e->next)
		kprintf(" %s", e->name);
	kprintf("\n");
}

/* ---- sys_open / sys_close --------------------------------------------- */

static struct stdata *stream_build(struct streamtab *drv_st, const char *name)
{
	struct stdata *sd = kmalloc(sizeof(*sd));
	queue_t *head_rq = kmalloc(sizeof(*head_rq));
	queue_t *head_wq = kmalloc(sizeof(*head_wq));
	queue_t *drv_rq  = kmalloc(sizeof(*drv_rq));
	queue_t *drv_wq  = kmalloc(sizeof(*drv_wq));
	if (!sd || !head_rq || !head_wq || !drv_rq || !drv_wq)
		return NULL;	/* leak; this path doesn't really happen */

	queue_init_pair(head_rq, head_wq, &sh_rinit,        &sh_winit);
	queue_init_pair(drv_rq,  drv_wq,  drv_st->st_rdinit, drv_st->st_wrinit);

	/* Wire the two-element stack: head <-> driver. */
	head_wq->q_next = drv_wq;	/* writes go DOWN: head -> drv */
	drv_rq->q_next  = head_rq;	/* reads go UP:    drv -> head */

	sd->sd_rq   = head_rq;
	sd->sd_wq   = head_wq;
	sd->sd_refs = 1;
	sd->sd_name = name;
	return sd;
}

int sys_open_impl(const char *name)
{
	if (!name)
		return -1;
	struct streamtab *st = streams_lookup(name);
	if (!st) {
		kprintf("sys_open: no such driver '%s'\n", name);
		return -1;
	}

	int fd = -1;
	for (int i = 0; i < FD_MAX; i++) {
		if (!fd_table[i].f_used) { fd = i; break; }
	}
	if (fd < 0)
		return -1;

	struct stdata *sd = stream_build(st, name);
	if (!sd)
		return -1;

	fd_table[fd].f_stream = sd;
	fd_table[fd].f_used   = 1;
	return fd;
}

int sys_close_impl(int fd)
{
	if (fd < 0 || fd >= FD_MAX || !fd_table[fd].f_used)
		return -1;
	fd_table[fd].f_used   = 0;
	fd_table[fd].f_stream = NULL;
	/* TODO: walk and free the queue chain + drain pending mblks. */
	return 0;
}

/* ---- sys_read / sys_write --------------------------------------------- */

long sys_write_impl(int fd, const void *buf, size_t len)
{
	if (fd < 0 || fd >= FD_MAX || !fd_table[fd].f_used)
		return -1;
	struct stdata *sd = fd_table[fd].f_stream;

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

long sys_read_impl(int fd, void *buf, size_t len)
{
	if (fd < 0 || fd >= FD_MAX || !fd_table[fd].f_used)
		return -1;
	struct stdata *sd = fd_table[fd].f_stream;

	mblk_t *mp = getq(sd->sd_rq);
	if (!mp)
		return 0;	/* "would block" -- no blocking yet */

	size_t avail   = msgdsize(mp);
	size_t to_copy = (avail < len) ? avail : len;
	kmemcpy(buf, mp->b_rptr, to_copy);

	/* Partial reads not handled; consume the whole mblk. */
	freemsg(mp);
	return (long)to_copy;
}

/* ---- sys_ioctl: I_PUSH / I_POP ---------------------------------------- */

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
	queue_t *old_top_rq = old_top_wq->q_link;	/* OTHERQ */

	/* Splice the new module in between head and old top. */
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
		return -1;	/* only thing below head IS the driver */

	queue_t *m_rq    = m_wq->q_link;
	queue_t *next_wq = m_wq->q_next;
	queue_t *next_rq = next_wq->q_link;

	sd->sd_wq->q_next = next_wq;
	next_rq->q_next   = sd->sd_rq;

	kfree(m_wq);
	kfree(m_rq);
	return 0;
}

long sys_ioctl_impl(int fd, int cmd, long arg)
{
	if (fd < 0 || fd >= FD_MAX || !fd_table[fd].f_used)
		return -1;
	struct stdata *sd = fd_table[fd].f_stream;

	switch (cmd) {
	case I_PUSH:
		return do_ipush(sd, (const char *)(uintptr_t)arg);
	case I_POP:
		return do_ipop(sd);
	default:
		return -1;
	}
}

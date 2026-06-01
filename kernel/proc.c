/*
 * kernel/proc.c -- procfs STREAMS chrdevs for introspection
 * =========================================================
 *
 * Each /proc entry is a STREAMS driver with a single trick: its
 * read-side qi_qopen formats a snapshot of some kernel data into
 * a buffer and putnext's it upstream as one or more mblks.  After
 * that, sys_read drains the head's queue like any other stream.
 *
 * This is the same pattern /dev/klog uses; the only thing /proc
 * adds is more files and a small text formatter (no kernel
 * vsnprintf -- we don't compile any libc).
 *
 * Layout that lands under /proc:
 *
 *   ps         every thread: tid, state, name (sorted by tid)
 *   meminfo    pmm free pages + kmalloc-slab totals (vmstat-ish)
 *   slabinfo   one row per size-cache: name, obj_size, free/total
 *   streams    one row per registered STREAMS module/driver
 *
 * Adding a new /proc/X entry is three things:
 *   1. claim a major in cdevsw.h
 *   2. write the qi_qopen that fills the queue
 *   3. cdev_register + vfs_mknod_chrdev in proc_init
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/cdevsw.h"
#include "kappara/kmem.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/proc.h"
#include "kappara/sched.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"
#include "kappara/string.h"
#include "kappara/vfs.h"

/* ---- Tiny text formatter into a fixed buffer ------------------------ */

#define PB_SIZE	2048

struct procbuf {
	char	buf[PB_SIZE];
	size_t	len;
};

static void pb_reset(struct procbuf *b)
{
	b->len = 0;
}

static void pb_putc(struct procbuf *b, char c)
{
	if (b->len < PB_SIZE) b->buf[b->len++] = c;
}

static void pb_str(struct procbuf *b, const char *s)
{
	while (*s) pb_putc(b, *s++);
}

static void pb_pad_str(struct procbuf *b, const char *s, int width)
{
	int n = 0;
	while (*s) { pb_putc(b, *s++); n++; }
	while (n++ < width) pb_putc(b, ' ');
}

static void pb_pad_dec(struct procbuf *b, unsigned long v, int width)
{
	/* Right-justify a decimal in `width` columns; ignore overflow. */
	char tmp[24];
	int  i = 0;
	if (v == 0) tmp[i++] = '0';
	while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
	while (i < width) { pb_putc(b, ' '); width--; }
	while (i--) pb_putc(b, tmp[i]);
}

/* Hand the formatted buffer up to the stream head as a single mblk
 * (assumes the snapshot fits in PB_SIZE, which is < kmalloc's
 * 2 KB cap so allocb is happy), then send M_HANGUP so the next
 * sys_read after the data is drained returns 0 instead of blocking
 * forever -- procfs entries are one-shot snapshots, not streams. */
static void pb_flush_to_q(struct procbuf *b, queue_t *q)
{
	if (b->len > 0) {
		mblk_t *mp = allocb(b->len, 0);
		if (mp) {
			kmemcpy(mp->b_wptr, b->buf, b->len);
			mp->b_wptr += b->len;
			putnext(q, mp);
		}
	}
	/* The hangup is a metadata mblk -- size 1 just because some
	 * older allocb variants reject size 0; the buffer is unused. */
	mblk_t *hup = allocb(1, 0);
	if (hup) {
		hup->b_datap->db_type = M_HANGUP;
		putnext(q, hup);
	}
}

/* ---- /proc/ps -------------------------------------------------------- */

static struct procbuf ps_pb;

static int proc_ps_qopen(queue_t *q)
{
	pb_reset(&ps_pb);
	pb_str(&ps_pb, "  TID  STATE   NAME\n");
	for (unsigned tid = 0; tid < kthread_max_tid(); tid++) {
		struct kthread *t = kthread_at(tid);
		if (!t) continue;
		pb_pad_dec(&ps_pb, t->tid, 5);
		pb_str(&ps_pb, "  ");
		pb_pad_str(&ps_pb, kthread_state_name(t->state), 8);
		pb_str(&ps_pb, t->name ? t->name : "?");
		pb_putc(&ps_pb, '\n');
	}
	pb_flush_to_q(&ps_pb, q);
	return 0;
}

/* ---- /proc/meminfo --------------------------------------------------- */

static struct procbuf mem_pb;

static int proc_mem_qopen(queue_t *q)
{
	pb_reset(&mem_pb);

	size_t free_pages = pmm_free_count();
	pb_str(&mem_pb, "pmm_free_pages:  ");
	pb_pad_dec(&mem_pb, free_pages, 8); pb_putc(&mem_pb, '\n');
	pb_str(&mem_pb, "pmm_free_KiB:    ");
	pb_pad_dec(&mem_pb, free_pages * 4, 8); pb_putc(&mem_pb, '\n');

	unsigned nc = kmem_num_size_caches();
	unsigned long total_free = 0, total_used = 0, total_bytes = 0;
	for (unsigned i = 0; i < nc; i++) {
		const struct kmem_cache *c = kmem_get_size_cache(i);
		total_free  += (unsigned long)c->free_objs;
		total_used  += (unsigned long)(c->total_objs - c->free_objs);
		total_bytes += (unsigned long)c->total_objs * c->obj_size;
	}
	pb_str(&mem_pb, "slab_objs_used:  ");
	pb_pad_dec(&mem_pb, total_used, 8); pb_putc(&mem_pb, '\n');
	pb_str(&mem_pb, "slab_objs_free:  ");
	pb_pad_dec(&mem_pb, total_free, 8); pb_putc(&mem_pb, '\n');
	pb_str(&mem_pb, "slab_bytes_tot:  ");
	pb_pad_dec(&mem_pb, total_bytes, 8); pb_putc(&mem_pb, '\n');

	pb_flush_to_q(&mem_pb, q);
	return 0;
}

/* ---- /proc/slabinfo -------------------------------------------------- */

static struct procbuf slab_pb;

static int proc_slab_qopen(queue_t *q)
{
	pb_reset(&slab_pb);
	pb_str(&slab_pb, "NAME        OBJSIZE   FREE  TOTAL\n");
	for (unsigned i = 0; i < kmem_num_size_caches(); i++) {
		const struct kmem_cache *c = kmem_get_size_cache(i);
		pb_pad_str(&slab_pb, c->name, 12);
		pb_pad_dec(&slab_pb, c->obj_size, 7);
		pb_str(&slab_pb, "  ");
		pb_pad_dec(&slab_pb, c->free_objs, 5);
		pb_pad_dec(&slab_pb, c->total_objs, 7);
		pb_putc(&slab_pb, '\n');
	}
	pb_flush_to_q(&slab_pb, q);
	return 0;
}

/* ---- /proc/streams --------------------------------------------------- */

static struct procbuf stream_pb;

static void stream_one_registered(const char *name, struct streamtab *st,
				   void *arg)
{
	(void)arg; (void)st;
	pb_str(&stream_pb, "  ");
	pb_str(&stream_pb, name);
	pb_putc(&stream_pb, '\n');
}

/* Walk one open stream's write-side queue stack from the head down,
 * stopping at the driver pair (which ends the chain for normal
 * streams; for pipes there's no driver, so we stop after the head). */
static void stream_one_open(struct stdata *sd, void *arg)
{
	(void)arg;
	const char *name = sd->sd_name ? sd->sd_name : "?";
	pb_str(&stream_pb, "  ");
	pb_pad_str(&stream_pb, name, 10);
	pb_str(&stream_pb, " refs=");
	pb_pad_dec(&stream_pb, sd->sd_refs, 2);
	pb_str(&stream_pb, "  stack:");
	/* Walk wq downward.  Each q's qinfo->minfo gives a module
	 * name.  Pipes (sd_drv_wq == NULL) just show the head; their
	 * sd_wq->q_next points at the peer's rq which isn't ours. */
	for (queue_t *q = sd->sd_wq; q;
	     q = (sd->sd_drv_wq && q == sd->sd_drv_wq) ? NULL : q->q_next) {
		const char *mn = (q->q_qinfo && q->q_qinfo->qi_minfo)
				 ? q->q_qinfo->qi_minfo->mi_idname : "?";
		pb_str(&stream_pb, " ");
		pb_str(&stream_pb, mn);
		if (q->q_next && (!sd->sd_drv_wq || q != sd->sd_drv_wq))
			pb_str(&stream_pb, " ->");
	}
	if (sd->sd_flags & SD_EOF) pb_str(&stream_pb, "  [EOF]");
	if (sd->sd_peer)           pb_str(&stream_pb, "  [pipe]");
	pb_putc(&stream_pb, '\n');
}

static int proc_stream_qopen(queue_t *q)
{
	pb_reset(&stream_pb);
	pb_str(&stream_pb, "registered modules/drivers:\n");
	streams_for_each(stream_one_registered, NULL);
	pb_str(&stream_pb, "\nopen streams:\n");
	streams_for_each_open(stream_one_open, NULL);
	pb_flush_to_q(&stream_pb, q);
	return 0;
}

/* ---- Read-side put: the head queues data; we never see reverse traffic. */

static int proc_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

/* Write side rejects: /proc is read-only. */
static int proc_wq_putp(queue_t *q, mblk_t *mp)
{
	(void)q;
	freemsg(mp);
	return -1;
}

static struct module_info proc_minfo = {
	.mi_idnum  = 300,
	.mi_idname = "proc",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

#define PROC_DRIVER(symbase, qopen_fn)					\
	static struct qinit symbase##_rinit = {				\
		.qi_putp  = proc_rq_putp,				\
		.qi_qopen = (qopen_fn),					\
		.qi_minfo = &proc_minfo,				\
	};								\
	static struct qinit symbase##_winit = {				\
		.qi_putp = proc_wq_putp, .qi_minfo = &proc_minfo,	\
	};								\
	static struct streamtab symbase##_streamtab = {			\
		.st_rdinit = &symbase##_rinit,				\
		.st_wrinit = &symbase##_winit,				\
	}

PROC_DRIVER(ps,     proc_ps_qopen);
PROC_DRIVER(mem,    proc_mem_qopen);
PROC_DRIVER(slab,   proc_slab_qopen);
PROC_DRIVER(stream, proc_stream_qopen);

void proc_init(void)
{
	cdev_register(CDEV_MAJ_PROC_PS,     "proc-ps",     &ps_streamtab);
	cdev_register(CDEV_MAJ_PROC_MEM,    "proc-mem",    &mem_streamtab);
	cdev_register(CDEV_MAJ_PROC_SLAB,   "proc-slab",   &slab_streamtab);
	cdev_register(CDEV_MAJ_PROC_STREAM, "proc-stream", &stream_streamtab);

	struct dentry *proc = vfs_mkdir(vfs_root(), "proc");
	vfs_mknod_chrdev(proc, "ps",       MKDEV(CDEV_MAJ_PROC_PS,     0));
	vfs_mknod_chrdev(proc, "meminfo",  MKDEV(CDEV_MAJ_PROC_MEM,    0));
	vfs_mknod_chrdev(proc, "slabinfo", MKDEV(CDEV_MAJ_PROC_SLAB,   0));
	vfs_mknod_chrdev(proc, "streams",  MKDEV(CDEV_MAJ_PROC_STREAM, 0));
}

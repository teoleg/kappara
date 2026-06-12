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

#include "kappara/io/cdevsw.h"
#include "kappara/core/ftrace.h"
#include "kappara/core/kmem.h"
#include "kappara/net/netif.h"
#include "kappara/net/slip.h"
#include "kappara/net/tcp.h"
#include "kappara/core/pmm.h"
#include "kappara/core/printk.h"
#include "kappara/fs/proc.h"
#include "kappara/proc/sched.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/core/string.h"
#include "kappara/fs/vfs.h"

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
	pb_str(&ps_pb, "  TID  STATE  PRI  CL   NAME\n");
	for (unsigned tid = 0; tid < kthread_max_tid(); tid++) {
		struct kthread *t = kthread_at(tid);
		if (!t) continue;
		pb_pad_dec(&ps_pb, t->tid, 5);
		pb_str(&ps_pb, "  ");
		pb_pad_str(&ps_pb, kthread_state_name(t->state), 6);
		/* Priority: idle threads carry KSCHED_PRI_IDLE = -1; show
		 * those as " - " so the column stays 3 wide without leaking
		 * a minus that looks like an unsigned underflow. */
		pb_putc(&ps_pb, ' ');
		if (t->t_pri == KSCHED_PRI_IDLE) {
			pb_str(&ps_pb, "  -");
		} else {
			pb_pad_dec(&ps_pb, (unsigned long)t->t_pri, 3);
		}
		pb_str(&ps_pb, "  ");
		pb_pad_str(&ps_pb, sclass[t->t_cid]->name, 4);
		pb_str(&ps_pb, " ");
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

/* ---- /proc/cpuload -------------------------------------------------- */

/*
 * One row per CPU: id, idle/busy, current dispatch-queue depth, and
 * the name of the thread that CPU is currently running.  Useful for
 * observing the push-side load balancer (`sched_get_cpu_info` in
 * kernel/sched.c) and noticing if one CPU is doing all the work.
 *
 * Snapshots are unlocked single-word reads -- a sample may be stale
 * by one instruction, which is fine for a diagnostic dump.
 */
static struct procbuf cpu_pb;

static int proc_cpu_qopen(queue_t *q)
{
	pb_reset(&cpu_pb);
	pb_str(&cpu_pb, "CPU  STATE  DISPQ  CURRENT\n");
	for (unsigned i = 0; i < sched_ncpu(); i++) {
		struct sched_cpu_info info;
		sched_get_cpu_info(i, &info);
		pb_pad_dec(&cpu_pb, info.cpu_id, 3);
		pb_str(&cpu_pb, "  ");
		pb_pad_str(&cpu_pb, info.idle ? "IDLE" : "BUSY", 5);
		pb_str(&cpu_pb, "  ");
		pb_pad_dec(&cpu_pb, info.dispq_len, 5);
		pb_str(&cpu_pb, "  ");
		pb_str(&cpu_pb, info.cur_name);
		pb_putc(&cpu_pb, '\n');
	}
	pb_flush_to_q(&cpu_pb, q);
	return 0;
}

/* ---- /proc/netif --------------------------------------------------- */
/*
 * One row per registered netif.  Columns:
 *   NAME    interface name (lo0, slip0, ...)
 *   FLAGS   text flags (UP for now; netifs that exist are always up)
 *   MTU     IP datagram cap in bytes
 *   IP      dotted-quad host address
 *   MASK    dotted-quad netmask
 *
 * The columns match what `ifconfig -a` would print on SVR4.  Real
 * Solaris shows per-zone state, multicast, broadcast, oerrors / ierrors;
 * we have none of that infrastructure yet.
 */

static struct procbuf netif_pb;

static void pb_dotted_quad(struct procbuf *b, uint32_t ip)
{
	pb_pad_dec(b, (ip >> 24) & 0xff, 0); pb_putc(b, '.');
	pb_pad_dec(b, (ip >> 16) & 0xff, 0); pb_putc(b, '.');
	pb_pad_dec(b, (ip >>  8) & 0xff, 0); pb_putc(b, '.');
	pb_pad_dec(b,  ip        & 0xff, 0);
}

static int netif_one_row(struct netif *nif, void *arg)
{
	struct procbuf *b = arg;
	pb_pad_str(b, nif->name, 8);
	pb_str(b, "UP    ");
	pb_pad_dec(b, nif->mtu, 6);
	pb_str(b, "  ");
	/* IP + mask in fixed-width slots so columns line up regardless
	 * of octet width. */
	size_t before = b->len;
	pb_dotted_quad(b, nif->ip);
	while ((b->len - before) < 16) pb_putc(b, ' ');
	pb_dotted_quad(b, nif->netmask);
	pb_putc(b, '\n');
	return 0;
}

static int proc_netif_qopen(queue_t *q)
{
	pb_reset(&netif_pb);
	pb_str(&netif_pb,
	       "NAME    FLAGS  MTU     IP              NETMASK\n");
	netif_for_each(netif_one_row, &netif_pb);
	pb_flush_to_q(&netif_pb, q);
	return 0;
}

/* ---- /proc/tcp ---------------------------------------------------- */
/*
 * One row per registered TCB.  Columns: state, local port, peer
 * addr:port, send-buf length, srtt and current RTO (both ms).
 */

static struct procbuf tcp_pb;

static void tcp_one_row(const struct tcp_tcb_view *v, void *arg)
{
	struct procbuf *b = arg;
	pb_pad_str(b, v->state_name, 14);
	pb_pad_dec(b, v->local_port, 6);
	pb_str(b, "  ");
	if (v->remote_ip) {
		pb_pad_dec(b, (v->remote_ip >> 24) & 0xff, 0); pb_putc(b, '.');
		pb_pad_dec(b, (v->remote_ip >> 16) & 0xff, 0); pb_putc(b, '.');
		pb_pad_dec(b, (v->remote_ip >>  8) & 0xff, 0); pb_putc(b, '.');
		pb_pad_dec(b,  v->remote_ip        & 0xff, 0); pb_putc(b, ':');
		pb_pad_dec(b, v->remote_port, 0);
	} else {
		pb_str(b, "-");
	}
	pb_str(b, "  sbuf=");
	pb_pad_dec(b, v->snd_buf_len, 0);
	pb_str(b, "  srtt=");
	pb_pad_dec(b, v->srtt_ms, 0);
	pb_str(b, "ms rto=");
	pb_pad_dec(b, v->rto_ms, 0);
	pb_str(b, "ms\n");
}

static int proc_tcp_qopen(queue_t *q)
{
	pb_reset(&tcp_pb);
	pb_str(&tcp_pb, "STATE         LPORT  PEER             EXTRAS\n");
	tcp_for_each_tcb(tcp_one_row, &tcp_pb);
	pb_flush_to_q(&tcp_pb, q);
	return 0;
}

/* ---- /proc/slip ---------------------------------------------------- */
/*
 * Per-line counters for the slip0 stream.  rx_bytes is what the mini-
 * UART rx kthread has pulled off the wire (regardless of framing);
 * frame counters come from the slip module's own state.
 */

static struct procbuf slip_pb;

static int proc_slip_qopen(queue_t *q)
{
	pb_reset(&slip_pb);
	pb_str(&slip_pb, "iface:  slip0  (mini-UART at 0x3F215040)\n");
	pb_str(&slip_pb, "rx_bytes_total:  ");
	pb_pad_dec(&slip_pb, slip_rx_byte_count(), 8); pb_putc(&slip_pb, '\n');
	struct slip_state_view sv;
	if (slip_get_stats(&sv) == 0) {
		pb_str(&slip_pb, "rx_frames:       ");
		pb_pad_dec(&slip_pb, sv.rx_frames,   8); pb_putc(&slip_pb, '\n');
		pb_str(&slip_pb, "rx_runts:        ");
		pb_pad_dec(&slip_pb, sv.rx_runts,    8); pb_putc(&slip_pb, '\n');
		pb_str(&slip_pb, "rx_overflow:     ");
		pb_pad_dec(&slip_pb, sv.rx_overflow, 8); pb_putc(&slip_pb, '\n');
		pb_str(&slip_pb, "tx_frames:       ");
		pb_pad_dec(&slip_pb, sv.tx_frames,   8); pb_putc(&slip_pb, '\n');
	} else {
		pb_str(&slip_pb, "(slip not initialised)\n");
	}
	pb_flush_to_q(&slip_pb, q);
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
PROC_DRIVER(cpu,    proc_cpu_qopen);
PROC_DRIVER(netif,  proc_netif_qopen);
PROC_DRIVER(procslip, proc_slip_qopen);
PROC_DRIVER(proctcp,  proc_tcp_qopen);

/* ---- /proc/ftrace --------------------------------------------------- */

/*
 * Unlike the other /proc entries we don't go through procbuf -- a full
 * trace dump is many KB, and ftrace_dump_to_q already streams its own
 * 1 KB mblks + an M_HANGUP at the end.  Just hand the read queue
 * straight to the tracer.
 */
static int proc_ftrace_qopen(queue_t *q)
{
	ftrace_dump_to_q(q, 0);
	return 0;
}

/*
 * Write side: ASCII control messages.  Recognised verbs:
 *
 *   off    disable recording (ring is preserved -- next cat shows it)
 *   on     re-enable recording
 *   reset  drop all events, head/total counters back to zero
 *
 * Match is on the first few non-whitespace characters so a trailing
 * newline from `echo "off"` is fine.  Anything else is silently
 * dropped (no error -- this is a debug tool, keep the noise floor
 * low).
 */
static int ftrace_match(const char *buf, size_t n, const char *verb)
{
	size_t i = 0;
	while (i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
	size_t j = 0;
	while (verb[j] && i + j < n && buf[i + j] == verb[j]) j++;
	if (verb[j] != '\0') return 0;
	/* Verb matched; next byte (if any) must be a separator. */
	size_t k = i + j;
	if (k >= n) return 1;
	char c = buf[k];
	return c == '\0' || c == '\n' || c == '\r' || c == ' ' || c == '\t';
}

static int proc_ftrace_wq_putp(queue_t *q, mblk_t *mp)
{
	(void)q;
	if (mp && mp->b_datap && mp->b_datap->db_type == M_DATA) {
		const char *buf = (const char *)mp->b_rptr;
		size_t      n   = (size_t)(mp->b_wptr - mp->b_rptr);
		if      (ftrace_match(buf, n, "off"))   ftrace_disable();
		else if (ftrace_match(buf, n, "on"))    ftrace_enable();
		else if (ftrace_match(buf, n, "reset")) ftrace_reset();
	}
	freemsg(mp);
	return 0;
}

static struct qinit ftrace_rinit = {
	.qi_putp  = proc_rq_putp,
	.qi_qopen = proc_ftrace_qopen,
	.qi_minfo = &proc_minfo,
};
static struct qinit ftrace_winit = {
	.qi_putp  = proc_ftrace_wq_putp,
	.qi_minfo = &proc_minfo,
};
static struct streamtab ftrace_streamtab = {
	.st_rdinit = &ftrace_rinit,
	.st_wrinit = &ftrace_winit,
};

void proc_init(void)
{
	cdev_register(CDEV_MAJ_PROC_PS,     "proc-ps",     &ps_streamtab);
	cdev_register(CDEV_MAJ_PROC_MEM,    "proc-mem",    &mem_streamtab);
	cdev_register(CDEV_MAJ_PROC_SLAB,   "proc-slab",   &slab_streamtab);
	cdev_register(CDEV_MAJ_PROC_STREAM, "proc-stream", &stream_streamtab);
	cdev_register(CDEV_MAJ_PROC_FTRACE, "proc-ftrace", &ftrace_streamtab);
	cdev_register(CDEV_MAJ_PROC_CPU,    "proc-cpu",    &cpu_streamtab);
	cdev_register(CDEV_MAJ_PROC_NETIF,  "proc-netif",  &netif_streamtab);
	cdev_register(CDEV_MAJ_PROC_SLIP,   "proc-slip",   &procslip_streamtab);
	cdev_register(CDEV_MAJ_PROC_TCP,    "proc-tcp",    &proctcp_streamtab);

	struct dentry *proc = vfs_mkdir(vfs_root(), "proc");
	vfs_mknod_chrdev(proc, "ps",       MKDEV(CDEV_MAJ_PROC_PS,     0));
	vfs_mknod_chrdev(proc, "meminfo",  MKDEV(CDEV_MAJ_PROC_MEM,    0));
	vfs_mknod_chrdev(proc, "slabinfo", MKDEV(CDEV_MAJ_PROC_SLAB,   0));
	vfs_mknod_chrdev(proc, "streams",  MKDEV(CDEV_MAJ_PROC_STREAM, 0));
	vfs_mknod_chrdev(proc, "ftrace",   MKDEV(CDEV_MAJ_PROC_FTRACE, 0));
	vfs_mknod_chrdev(proc, "cpuload",  MKDEV(CDEV_MAJ_PROC_CPU,    0));
	vfs_mknod_chrdev(proc, "netif",    MKDEV(CDEV_MAJ_PROC_NETIF,  0));
	vfs_mknod_chrdev(proc, "slip",     MKDEV(CDEV_MAJ_PROC_SLIP,   0));
	vfs_mknod_chrdev(proc, "tcp",      MKDEV(CDEV_MAJ_PROC_TCP,    0));
}

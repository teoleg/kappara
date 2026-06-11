/*
 * kernel/streams.c -- SVR4 STREAMS framework, core
 * ================================================
 *
 * What this file is
 * -----------------
 * The bottom layer of the STREAMS subsystem: message allocation
 * and a simple per-queue deferred-message list.  See the comment
 * at the top of include/kappara/io/streams.h for the framework
 * overview and the data-structure diagrams.
 *
 * What's in here
 * --------------
 *   streams_init       create the slab caches for mblk/dblk/queue
 *   allocb             allocate a message with an attached buffer
 *   freeb              drop one mblk's ref to the dblk; free if last
 *   freemsg            walk b_cont, freeb everything
 *   msgdsize           total size of an mblk chain
 *   queue_init_pair    initialise a paired (read, write) queue
 *   putq               append mp to q's deferred message list
 *   getq               pop the head of q's deferred message list
 *   putnext            call q->q_next's put procedure
 *
 * What's NOT in here yet
 * ----------------------
 *   * Service procedures (qenable / runqueues): putq currently never
 *     defers; the upstream module's putp is expected to call its
 *     downstream module's putp directly via putnext.  When we add
 *     real flow control and async drain we'll grow a tiny scheduler
 *     here.
 *   * dupb / dupmsg (cheap message copying via dblk refcount).  The
 *     ref-counting machinery is in place; just no wrapper yet.
 *   * pullupmsg / linkb / unlinkb -- the message-mutation helpers.
 *   * Stream head, open/close, I_PUSH/I_POP.  Higher-level work that
 *     sits on top of these primitives.
 *
 * Why three slab caches
 * ---------------------
 *   mblk_cache    fixed-size mblk_t (very high churn -- one per msg)
 *   dblk_cache    fixed-size dblk_t (one per buffer, may be shared)
 *   queue_cache   queue_t (one per module pair direction)
 *
 * The data buffer attached to each dblk is plain kmalloc'd, so its
 * size can be anything up to the kmalloc cap (2048 bytes today).
 *
 * Refcount model
 * --------------
 *   allocb       allocates buffer + dblk + mblk, sets db_ref = 1.
 *   freeb        --db_ref; if zero, free buffer and dblk; always
 *                free the mblk.
 *   freemsg      walk b_cont, freeb each link.
 *
 * Eventually a `dupb(mblk)` will return a NEW mblk pointing at the
 * SAME dblk (incrementing db_ref).  Two views of the same payload,
 * one buffer.  This is the SVR4 "message duplication is cheap"
 * property and it's what makes STREAMS' fan-out reasonable.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/core/kmem.h"
#include "kappara/core/printk.h"
#include "kappara/proc/sched.h"	/* curcpu, cpu_srvq_* */
#include "kappara/core/spinlock.h"
#include "kappara/io/streams.h"
#include "kappara/core/string.h"

static struct kmem_cache mblk_cache;
static struct kmem_cache dblk_cache;
static struct kmem_cache queue_cache;

void streams_init(void)
{
	kmem_cache_init(&mblk_cache,  "mblk",  sizeof(mblk_t));
	kmem_cache_init(&dblk_cache,  "dblk",  sizeof(dblk_t));
	kmem_cache_init(&queue_cache, "queue", sizeof(queue_t));

	kprintf("streams: caches up  mblk=%lu  dblk=%lu  queue=%lu  bytes\n",
		(unsigned long)sizeof(mblk_t),
		(unsigned long)sizeof(dblk_t),
		(unsigned long)sizeof(queue_t));
}

/* ---- Message allocation ------------------------------------------------- */

mblk_t *allocb(size_t size, unsigned pri)
{
	(void)pri;	/* priority bands not honoured yet */

	dblk_t *db = kmem_cache_alloc(&dblk_cache);
	if (!db)
		return NULL;

	unsigned char *buf = NULL;
	if (size > 0) {
		buf = kmalloc(size);
		if (!buf) {
			kmem_cache_free(&dblk_cache, db);
			return NULL;
		}
	}

	mblk_t *mp = kmem_cache_alloc(&mblk_cache);
	if (!mp) {
		if (buf)
			kfree(buf);
		kmem_cache_free(&dblk_cache, db);
		return NULL;
	}

	db->db_base = buf;
	db->db_lim  = buf + size;
	db->db_ref  = 1;
	db->db_type = M_DATA;

	mp->b_next  = NULL;
	mp->b_prev  = NULL;
	mp->b_cont  = NULL;
	mp->b_rptr  = buf;
	mp->b_wptr  = buf;
	mp->b_datap = db;
	mp->b_band  = 0;
	mp->b_flag  = 0;

	return mp;
}

void freeb(mblk_t *mp)
{
	if (!mp)
		return;
	dblk_t *db = mp->b_datap;
	if (--db->db_ref == 0) {
		if (db->db_base)
			kfree(db->db_base);
		kmem_cache_free(&dblk_cache, db);
	}
	kmem_cache_free(&mblk_cache, mp);
}

void freemsg(mblk_t *mp)
{
	while (mp) {
		mblk_t *next = mp->b_cont;
		freeb(mp);
		mp = next;
	}
}

size_t msgdsize(const mblk_t *mp)
{
	size_t n = 0;
	while (mp) {
		n += (size_t)(mp->b_wptr - mp->b_rptr);
		mp = mp->b_cont;
	}
	return n;
}

/* SVR4 dupb: allocate a new mblk_t referencing the same dblk.  The
 * buffer is NOT copied -- both mblks point at the same bytes -- but
 * the dblk's refcount goes up by one, so freeb on either correctly
 * decrements without freeing the shared buffer until the last
 * reference goes away.  b_rptr / b_wptr / b_band / b_flag are
 * copied so the new mblk sees the same window into the data; b_next
 * / b_prev / b_cont are reset.
 *
 * Used by stream muxes (IP demux multicast) to fan a single arriving
 * packet out to several bound upper streams without N copies of the
 * payload bytes -- only N tiny mblk headers. */
mblk_t *dupb(mblk_t *mp)
{
	if (!mp || !mp->b_datap)
		return NULL;
	mblk_t *cp = kmem_cache_alloc(&mblk_cache);
	if (!cp)
		return NULL;
	cp->b_next  = NULL;
	cp->b_prev  = NULL;
	cp->b_cont  = NULL;
	cp->b_rptr  = mp->b_rptr;
	cp->b_wptr  = mp->b_wptr;
	cp->b_datap = mp->b_datap;
	cp->b_band  = mp->b_band;
	cp->b_flag  = mp->b_flag;
	mp->b_datap->db_ref++;
	return cp;
}

/* SVR4 dupmsg: dupb every link in the b_cont chain.  Returns the
 * head of a parallel chain, or NULL on alloc failure (partial chains
 * are freed -- caller never sees a half-built message). */
mblk_t *dupmsg(mblk_t *mp)
{
	mblk_t *head = NULL;
	mblk_t *tail = NULL;
	while (mp) {
		mblk_t *cp = dupb(mp);
		if (!cp) {
			if (head) freemsg(head);
			return NULL;
		}
		if (!head) head = cp;
		if (tail)  tail->b_cont = cp;
		tail = cp;
		mp = mp->b_cont;
	}
	return head;
}

/* ---- Queue plumbing ----------------------------------------------------- */

void queue_init_pair(queue_t *rq, queue_t *wq,
		     struct qinit *rinit, struct qinit *winit)
{
	kmemset(rq, 0, sizeof(*rq));
	kmemset(wq, 0, sizeof(*wq));

	rq->q_qinfo = rinit;
	wq->q_qinfo = winit;

	rq->q_link = wq;
	wq->q_link = rq;

	if (rinit && rinit->qi_minfo) {
		rq->q_minpsz = rinit->qi_minfo->mi_minpsz;
		rq->q_maxpsz = rinit->qi_minfo->mi_maxpsz;
		rq->q_hiwat  = rinit->qi_minfo->mi_hiwat;
		rq->q_lowat  = rinit->qi_minfo->mi_lowat;
	}
	if (winit && winit->qi_minfo) {
		wq->q_minpsz = winit->qi_minfo->mi_minpsz;
		wq->q_maxpsz = winit->qi_minfo->mi_maxpsz;
		wq->q_hiwat  = winit->qi_minfo->mi_hiwat;
		wq->q_lowat  = winit->qi_minfo->mi_lowat;
	}
}

void putq(queue_t *q, mblk_t *mp)
{
	mp->b_next = NULL;
	mp->b_prev = q->q_last;
	if (q->q_last)
		q->q_last->b_next = mp;
	else
		q->q_first = mp;
	q->q_last = mp;
	q->q_count += msgdsize(mp);

	if ((long)q->q_count >= q->q_hiwat)
		q->q_flag |= QFULL;
}

void putbq(queue_t *q, mblk_t *mp)
{
	mp->b_prev = NULL;
	mp->b_next = q->q_first;
	if (q->q_first)
		q->q_first->b_prev = mp;
	else
		q->q_last = mp;
	q->q_first = mp;
	q->q_count += msgdsize(mp);
	/* QFULL is not toggled here: we're putting back what we just
	 * pulled out, so the original ceiling state was already correct. */
}

mblk_t *getq(queue_t *q)
{
	mblk_t *mp = q->q_first;
	if (!mp)
		return NULL;
	q->q_first = mp->b_next;
	if (q->q_first)
		q->q_first->b_prev = NULL;
	else
		q->q_last = NULL;
	mp->b_next = NULL;
	mp->b_prev = NULL;

	q->q_count -= msgdsize(mp);
	if ((long)q->q_count <= q->q_lowat)
		q->q_flag &= ~QFULL;

	return mp;
}

int putnext(queue_t *q, mblk_t *mp)
{
	queue_t *next = q->q_next;
	if (!next || !next->q_qinfo || !next->q_qinfo->qi_putp) {
		freemsg(mp);
		return -1;
	}
	return next->q_qinfo->qi_putp(next, mp);
}

/* SVR4 put(): invoke a queue's own qi_putp.  Unlike putnext (which
 * traverses q->q_next), put delivers directly to the target queue
 * without following any chain.  This is the mux primitive: a driver
 * like IP whose rput needs to cross a stream boundary (to deliver
 * a demultiplexed packet to an upper protocol's queue stashed at
 * bind time) calls put on the foreign queue.  putnext would either
 * follow the wrong chain or jump into the destination's "next" hop;
 * put hits exactly the queue we have a pointer to. */
int put(queue_t *q, mblk_t *mp)
{
	if (!q || !q->q_qinfo || !q->q_qinfo->qi_putp) {
		freemsg(mp);
		return -1;
	}
	return q->q_qinfo->qi_putp(q, mp);
}

/* ---- Service procedure scheduling -------------------------------------- */

/*
 * Per-CPU service-procedure runqueue.  Each cpu_t (sched.h) carries
 * cpu_srvq_head + cpu_srvq_tail + cpu_srvq_lock.  qenable pushes
 * onto the CALLING CPU's runqueue; streams_run drains the CALLING
 * CPU's runqueue.
 *
 * This is the SVR4-natural alignment for STREAMS on SMP: service
 * work follows the producer.  A driver that calls qenable from an
 * IRQ pinned to CPU N has its srvp drained on CPU N -- the data
 * cache locality of the mblks just stays where the work was done.
 * No cross-CPU runqueue contention in the common case.
 *
 * Threaded via q->q_runlink; QENAB in q_flag is the "I am already
 * on SOMEONE's runqueue" sentinel.  Note QENAB doesn't say WHICH
 * CPU; the migration story (qenable bound to CPU A while srvp
 * drains on CPU B because the queue moved) is a follow-up.  For
 * now a queue stays on whatever CPU first qenable'd it.
 */
void qenable(queue_t *q)
{
	if (!q || !q->q_qinfo || !q->q_qinfo->qi_srvp)
		return;
	struct cpu *c = curcpu();
	unsigned long f = spin_lock_irq_save(&c->cpu_srvq_lock);
	if (q->q_flag & QENAB) {
		spin_unlock_irq_restore(&c->cpu_srvq_lock, f);
		return;
	}
	q->q_flag |= QENAB;
	q->q_runlink = NULL;
	if (c->cpu_srvq_tail)
		c->cpu_srvq_tail->q_runlink = q;
	else
		c->cpu_srvq_head = q;
	c->cpu_srvq_tail = q;
	spin_unlock_irq_restore(&c->cpu_srvq_lock, f);
}

/*
 * Per-CPU re-entrancy guard.  Now that syscalls run with IRQs
 * unmasked the timer can preempt a thread that's already inside
 * streams_run (via sys_yield); without this the IRQ-driven
 * sched_tick would walk the same runqueue from underneath us.
 * Keyed off cpu_id so different CPUs' streams_run calls don't
 * exclude each other.
 */
static volatile int streams_running[4];

void streams_run(void)
{
	struct cpu *c = curcpu();
	if (streams_running[c->cpu_id])
		return;
	streams_running[c->cpu_id] = 1;

	for (;;) {
		unsigned long f = spin_lock_irq_save(&c->cpu_srvq_lock);
		queue_t *q = c->cpu_srvq_head;
		if (!q) {
			spin_unlock_irq_restore(&c->cpu_srvq_lock, f);
			break;
		}
		c->cpu_srvq_head = q->q_runlink;
		if (!c->cpu_srvq_head)
			c->cpu_srvq_tail = NULL;
		q->q_runlink = NULL;
		q->q_flag &= ~QENAB;
		spin_unlock_irq_restore(&c->cpu_srvq_lock, f);

		/*
		 * srvp may call putnext -> downstream putp -> putq +
		 * qenable, which (on this same CPU) appends to our own
		 * runqueue.  That's the whole point of having a
		 * worklist: those appends get picked up by the next
		 * iteration.  Call srvp with NO locks held.
		 */
		if (q->q_qinfo->qi_srvp)
			q->q_qinfo->qi_srvp(q);
	}

	streams_running[c->cpu_id] = 0;
}

/*
 * kernel/streams.c -- SVR4 STREAMS framework, core
 * ================================================
 *
 * What this file is
 * -----------------
 * The bottom layer of the STREAMS subsystem: message allocation
 * and a simple per-queue deferred-message list.  See the comment
 * at the top of include/kappara/streams.h for the framework
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

#include "kappara/kmem.h"
#include "kappara/printk.h"
#include "kappara/streams.h"
#include "kappara/string.h"

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

/* ---- Service procedure scheduling -------------------------------------- */

/*
 * runqueue: a singly-linked list of queues whose service procedures
 * we owe a call to.  Threaded via q->q_runlink; QENAB in q_flag is
 * the "I am already on the runqueue" sentinel that makes qenable
 * idempotent.
 */
static queue_t *runq_head;
static queue_t *runq_tail;

void qenable(queue_t *q)
{
	if (!q || !q->q_qinfo || !q->q_qinfo->qi_srvp)
		return;
	if (q->q_flag & QENAB)
		return;
	q->q_flag |= QENAB;
	q->q_runlink = NULL;
	if (runq_tail)
		runq_tail->q_runlink = q;
	else
		runq_head = q;
	runq_tail = q;
}

void streams_run(void)
{
	while (runq_head) {
		queue_t *q = runq_head;
		runq_head = q->q_runlink;
		if (!runq_head)
			runq_tail = NULL;
		q->q_runlink = NULL;
		q->q_flag &= ~QENAB;

		/*
		 * srvp may call putnext -> downstream putp -> putq + qenable,
		 * which appends to the runqueue we're walking.  That's the
		 * whole point of having a worklist: those appends get
		 * picked up by the next iteration.
		 */
		if (q->q_qinfo->qi_srvp)
			q->q_qinfo->qi_srvp(q);
	}
}

/*
 * kernel/io/buf.c -- block-IO buffer cache
 *
 * SVR4 verbs: getblk / brelse / bread / bwrite / bdwrite.  See
 * include/kappara/fs/buf.h for the API + flag set.
 *
 * Phase S1.
 *
 * Data structures:
 *   - bufs[BUF_POOL_SIZE]: pool of struct buf headers
 *     allocated at buf_init time.  Each carries a 4 KiB data page
 *     pulled from pmm.
 *   - hash[BUF_HASH_BUCKETS]: bucket-per-hash chain.  Hash is
 *     dev * 31 + blkno, masked to BUF_HASH_BUCKETS-1.
 *   - lru: doubly-linked list of buffers NOT currently B_BUSY.
 *     Head = next victim; tail = most recently used.
 *
 * Concurrency: buf_lock guards everything.  Held briefly around
 * lookup / state transitions.  Released across the actual block
 * I/O so a slow driver doesn't lock out other lookups.
 *
 * Locking + sleep on B_BUSY:
 *   - When getblk finds a buf with B_BUSY set, it parks on
 *     bp->wait and the releaser wakes it on brelse.  Sleep-on-locked
 *     pattern from phase 5 (sq_lock interlock) avoids the lost-
 *     wakeup race.
 */

#include <stdint.h>

#include "kappara/fs/bdevsw.h"
#include "kappara/fs/buf.h"
#include "kappara/core/kmem.h"
#include "kappara/core/pmm.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"

static struct buf       buf_pool[BUF_POOL_SIZE];
static struct buf      *buf_hash[BUF_HASH_BUCKETS];
static struct buf      *lru_head, *lru_tail;
static spinlock_t       buf_lock = SPINLOCK_INIT;

static unsigned buf_hash_idx(dev_t dev, uint64_t blkno)
{
	return (unsigned)((((uint64_t)dev * 31) ^ blkno)
	                  & (BUF_HASH_BUCKETS - 1));
}

static void lru_unlink(struct buf *bp)
{
	if (bp->lru_prev) bp->lru_prev->lru_next = bp->lru_next;
	else              lru_head = bp->lru_next;
	if (bp->lru_next) bp->lru_next->lru_prev = bp->lru_prev;
	else              lru_tail = bp->lru_prev;
	bp->lru_prev = bp->lru_next = 0;
}

static void lru_insert_tail(struct buf *bp)
{
	bp->lru_prev = lru_tail;
	bp->lru_next = 0;
	if (lru_tail) lru_tail->lru_next = bp;
	else          lru_head = bp;
	lru_tail = bp;
}

static void hash_insert(struct buf *bp)
{
	unsigned h = buf_hash_idx(bp->b_dev, bp->b_blkno);
	bp->hash_next = buf_hash[h];
	buf_hash[h] = bp;
}

static void hash_remove(struct buf *bp)
{
	unsigned h = buf_hash_idx(bp->b_dev, bp->b_blkno);
	struct buf **link = &buf_hash[h];
	while (*link && *link != bp) link = &(*link)->hash_next;
	if (*link == bp) *link = bp->hash_next;
	bp->hash_next = 0;
}

static struct buf *hash_lookup(dev_t dev, uint64_t blkno)
{
	unsigned h = buf_hash_idx(dev, blkno);
	for (struct buf *bp = buf_hash[h]; bp; bp = bp->hash_next) {
		if (bp->b_dev == dev && bp->b_blkno == blkno) return bp;
	}
	return 0;
}

void buf_init(void)
{
	for (unsigned i = 0; i < BUF_POOL_SIZE; i++) {
		struct buf *bp = &buf_pool[i];
		/* 512-byte slots fit in the slab's size-512 cache.  pmm_alloc
		 * would waste 7/8 of every 4 KiB page on us, and we want the
		 * pool dense enough that real workloads (kfs walks, FTP STORs)
		 * don't keep evicting each other. */
		bp->b_data = kmalloc(BUF_BLOCK_SIZE);
		if (!bp->b_data) {
			kprintf("buf_init: slab exhausted at slot %u\n", i);
			break;
		}
		bp->b_addr  = bp->b_data;	/* cache flow: addr == data */
		bp->b_dev   = (dev_t)-1;	/* never matches a real lookup */
		bp->b_blkno = 0;
		bp->b_bcount = BUF_BLOCK_SIZE;
		bp->b_flags = 0;
		bp->b_error = 0;
		bp->b_iodone = NULL;
		bp->b_private = NULL;
		bp->wait    = (struct wait_queue)WAIT_QUEUE_INIT;
		lru_insert_tail(bp);
	}
	kprintf("buf: cache initialised (%u slots * %u bytes = %u KiB)\n",
	        BUF_POOL_SIZE, BUF_BLOCK_SIZE,
	        (BUF_POOL_SIZE * BUF_BLOCK_SIZE) >> 10);
}

/* Pick the LRU head as a victim.  If it's dirty, write it back
 * before reusing.  Returns the victim (BUSY, ready for new use)
 * or NULL if the cache is so contested every buffer is busy --
 * we kprintf and the caller retries. */
static struct buf *evict_one(void)
{
	struct buf *bp = lru_head;
	if (!bp) return 0;
	lru_unlink(bp);

	if (bp->b_flags & B_DELWRI) {
		/* Write back synchronously before reusing.  Release
		 * the cache lock first since this hits the driver. */
		spin_unlock(&buf_lock);
		(void)bwrite(bp);
		spin_lock(&buf_lock);
		/* bwrite clears B_DELWRI + B_BUSY; we want it busy
		 * again so the eviction story stays consistent. */
	}
	if (bp->b_dev != (dev_t)-1) hash_remove(bp);
	bp->b_flags = B_BUSY;
	return bp;
}

struct buf *getblk(dev_t dev, uint64_t blkno)
{
	for (;;) {
		unsigned long flags = spin_lock_irq_save(&buf_lock);
		struct buf *bp = hash_lookup(dev, blkno);
		if (bp) {
			if (bp->b_flags & B_BUSY) {
				/* Sleep until the owner brelses. */
				kthread_sleep_on_locked(&bp->wait, flags);
				continue;
			}
			lru_unlink(bp);
			bp->b_flags |= B_BUSY;
			spin_unlock_irq_restore(&buf_lock, flags);
			return bp;
		}
		/* Miss -- evict + reuse. */
		bp = evict_one();
		if (!bp) {
			spin_unlock_irq_restore(&buf_lock, flags);
			kprintf("getblk: pool exhausted\n");
			return 0;
		}
		bp->b_dev    = dev;
		bp->b_blkno  = blkno;
		bp->b_bcount = BUF_BLOCK_SIZE;
		bp->b_flags  = B_BUSY;	/* clear VALID/DELWRI/etc */
		bp->b_error  = 0;
		bp->b_iodone = NULL;
		bp->b_private = NULL;
		hash_insert(bp);
		spin_unlock_irq_restore(&buf_lock, flags);
		return bp;
	}
}

void brelse(struct buf *bp)
{
	if (!bp) return;
	unsigned long flags = spin_lock_irq_save(&buf_lock);
	bp->b_flags &= ~B_BUSY;
	lru_insert_tail(bp);
	spin_unlock_irq_restore(&buf_lock, flags);
	/* Wake any thread sleeping on B_BUSY clear. */
	kthread_wake_all(&bp->wait);
}

/*
 * Issue `bp` to bdevsw[major].d_strategy and wait for the driver to
 * call biodone().  Sync drivers (everything we ship today) call
 * biodone before returning from strategy, so biowait is a fast no-op;
 * the call survives async backends without restructuring callers.
 *
 * Returns 0 on success, -1 if the driver set B_ERROR (or the major
 * has no strategy registered).
 */
static int strategy_and_wait(struct buf *bp)
{
	struct bdev_entry *e = bdev_lookup(MAJOR(bp->b_dev));
	if (!e || !e->d_strategy) {
		bp->b_flags |= B_ERROR | B_DONE;
		bp->b_error = -1;
		return -1;
	}
	/* Clear B_DONE before submitting so biowait knows to actually
	 * sleep on async backends.  The status bits get repopulated by
	 * the driver via biodone. */
	bp->b_flags &= ~(B_DONE | B_ERROR);
	bp->b_resid  = 0;
	e->d_strategy(MINOR(bp->b_dev), bp);
	(void)biowait(bp);
	return (bp->b_flags & B_ERROR) ? -1 : 0;
}

struct buf *bread(dev_t dev, uint64_t blkno)
{
	struct buf *bp = getblk(dev, blkno);
	if (!bp) return 0;
	if (bp->b_flags & B_VALID) return bp;	/* cache hit */

	bp->b_addr   = bp->b_data;
	bp->b_bcount = BUF_BLOCK_SIZE;
	bp->b_flags |= B_READ;
	if (strategy_and_wait(bp) < 0) {
		brelse(bp);
		return 0;
	}
	bp->b_flags |= B_VALID;
	bp->b_flags &= ~B_READ;
	return bp;
}

int bwrite(struct buf *bp)
{
	if (!bp) return -1;

	bp->b_addr   = bp->b_data;
	bp->b_bcount = BUF_BLOCK_SIZE;
	bp->b_flags |= B_WRITE;
	int rc = strategy_and_wait(bp);
	bp->b_flags &= ~B_WRITE;
	if (rc < 0) {
		brelse(bp);
		return -1;
	}
	bp->b_flags |= B_VALID;
	bp->b_flags &= ~B_DELWRI;
	brelse(bp);
	return 0;
}

/*
 * Driver-side completion.  Sync drivers call this inside d_strategy
 * before returning; async drivers call it from their completion
 * IRQ.  Sets B_DONE (always), runs b_iodone if installed, wakes
 * anyone in biowait.
 */
void biodone(struct buf *bp)
{
	if (!bp) return;
	unsigned long flags = spin_lock_irq_save(&buf_lock);
	bp->b_flags |= B_DONE;
	spin_unlock_irq_restore(&buf_lock, flags);
	if (bp->b_iodone) bp->b_iodone(bp);
	kthread_wake_all(&bp->wait);
}

int biowait(struct buf *bp)
{
	if (!bp) return -1;
	/* Sync drivers have already set B_DONE before strategy returned;
	 * the loop is here for the async path.  Re-checking under the
	 * lock avoids the lost-wakeup race between the IRQ handler's
	 * biodone and our sleep. */
	for (;;) {
		unsigned long flags = spin_lock_irq_save(&buf_lock);
		if (bp->b_flags & B_DONE) {
			spin_unlock_irq_restore(&buf_lock, flags);
			break;
		}
		kthread_sleep_on_locked(&bp->wait, flags);
	}
	return (bp->b_flags & B_ERROR) ? -1 : 0;
}

void bdwrite(struct buf *bp)
{
	if (!bp) return;
	bp->b_flags |= B_DELWRI | B_VALID;
	brelse(bp);
}

void bawrite(struct buf *bp)
{
	/* Async-write hint: today this is just brelse.  When bdflush
	 * exists it'll mark B_DELWRI + nudge the flush thread. */
	bdwrite(bp);
}

void buf_stats(void)
{
	unsigned dirty = 0, valid = 0, busy = 0;
	unsigned long flags = spin_lock_irq_save(&buf_lock);
	for (unsigned i = 0; i < BUF_POOL_SIZE; i++) {
		unsigned f = buf_pool[i].b_flags;
		if (f & B_BUSY)    busy++;
		if (f & B_DELWRI)  dirty++;
		if (f & B_VALID)   valid++;
	}
	spin_unlock_irq_restore(&buf_lock, flags);
	kprintf("buf: %u slots, %u busy, %u valid, %u dirty\n",
	        BUF_POOL_SIZE, busy, valid, dirty);
}

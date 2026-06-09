/*
 * include/kappara/buf.h -- block-IO buffer cache
 *
 * The fs-agnostic byte cache that sits between filesystems and
 * block-device drivers.  Filesystems call bread(dev, blkno) to
 * read a block; the cache either hands back an in-memory copy or
 * blocks while the driver pulls it from the disk.  bwrite(buf)
 * commits a dirty buffer; bdwrite(buf) marks it for delayed
 * write-back so the next bdflush sweep handles it.
 *
 * Phase S1.  SVR4 buffer-cache shape with the standard verbs;
 * implementation in uts/os/io/buf.c.
 *
 * Memory model:
 *   - A fixed pool of BUF_POOL_SIZE struct buf headers.
 *   - Each buf owns one BUF_BLOCK_SIZE data page.
 *   - Lookup via hash on (dev, blkno).
 *   - Eviction: pick the head of the LRU free list.
 *
 * Concurrency:
 *   - The pool + hash + LRU are protected by buf_lock (a single
 *     global spinlock for now).  Per-bucket locks would help under
 *     a real multi-process workload but aren't needed yet.
 *   - A buf marked B_BUSY is owned by the caller; concurrent
 *     getblk() on the same (dev, blkno) sleeps until the owner
 *     calls brelse().
 *
 * Flags:
 *   B_READ     I/O direction: read into b_data on miss.
 *   B_WRITE    I/O direction: write b_data out on commit.
 *   B_DONE     I/O completed (successfully or otherwise).
 *   B_ERROR    I/O failed.
 *   B_BUSY     Buffer is checked out; another getblk() must wait.
 *   B_DELWRI   Delayed write -- dirty, will be written by bdflush.
 *   B_VALID    Data in b_data reflects the on-disk content.
 */
#ifndef KAPPARA_BUF_H
#define KAPPARA_BUF_H

#include <stdint.h>

#include "kappara/cdevsw.h"
#include "kappara/sched.h"
#include "kappara/spinlock.h"

#define BUF_BLOCK_SIZE	4096		/* bytes per cache slot       */
#define BUF_POOL_SIZE	32		/* total cache slots          */
#define BUF_HASH_BUCKETS 16

#define B_READ		0x0001
#define B_WRITE		0x0002
#define B_DONE		0x0004
#define B_ERROR		0x0008
#define B_BUSY		0x0010
#define B_DELWRI	0x0020
#define B_VALID		0x0040

struct buf {
	dev_t      b_dev;
	uint64_t   b_blkno;
	unsigned   b_size;	/* bytes in b_data (== BUF_BLOCK_SIZE) */
	unsigned   b_flags;
	void      *b_data;	/* BUF_BLOCK_SIZE bytes, page-aligned   */

	/* LRU + hash linkage */
	struct buf *lru_prev;
	struct buf *lru_next;
	struct buf *hash_next;

	struct wait_queue wait;	/* sleepers waiting for B_BUSY clear   */
};

/* Initialise the buffer cache.  Allocates the pool of buf headers
 * + their data pages from pmm.  Called once from kmain, after
 * pmm_init but before any fs touches it. */
void buf_init(void);

/* Find (or allocate) the buf for (dev, blkno).  On return the buf
 * is marked B_BUSY -- caller owns it until brelse().  If the
 * cache had to evict a dirty buffer to make room, that buffer is
 * written back synchronously first. */
struct buf *getblk(dev_t dev, uint64_t blkno);

/* Release a buf back to the LRU free list.  Wakes any thread
 * sleeping on it. */
void brelse(struct buf *bp);

/* Blocking read: getblk + (if !B_VALID) driver read.  Returns the
 * busy buf, or NULL on driver error. */
struct buf *bread(dev_t dev, uint64_t blkno);

/* Synchronous write: pushes b_data to the device, marks B_VALID,
 * clears B_DELWRI.  Returns 0 on success, -1 on driver error. */
int  bwrite(struct buf *bp);

/* Delayed write: mark B_DELWRI; bdflush will commit later.  Cheap. */
void bdwrite(struct buf *bp);

/* Optional callback: hint that this buf is unlikely to be reused
 * soon.  Just brelse for now -- the LRU does the work. */
void bawrite(struct buf *bp);

/* Diagnostic: dump cache stats via kprintf. */
void buf_stats(void);

/* In-memory test block device + selftest harness.  bram_init
 * registers BDEV_MAJ_RAM minor 0 backed by a static 256 KiB
 * buffer; buf_selftest exercises the cache through it. */
void bram_init    (void);
void buf_selftest (void);

#endif

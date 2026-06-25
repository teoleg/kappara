/*
 * include/kappara/fs/buf.h -- block-IO buffer cache
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

#include "kappara/io/cdevsw.h"
#include "kappara/proc/sched.h"
#include "kappara/core/spinlock.h"

/* Cache slot size = device logical sector size.  512 B matches
 * kfs's on-disk layout and every block device we ship today
 * (ramdisk, nvme); the 4096 B legacy value pre-dated the kfs/nvme
 * shape and only made sense when the bram test driver was the
 * sole consumer. */
#define BUF_BLOCK_SIZE	512
#define BUF_POOL_SIZE	64		/* total cache slots          */
#define BUF_HASH_BUCKETS 16

#define B_READ		0x0001	/* direction: device -> b_addr  */
#define B_WRITE		0x0002	/* direction: b_addr -> device  */
#define B_DONE		0x0004	/* I/O completed (any outcome)  */
#define B_ERROR		0x0008	/* B_DONE + transfer failed     */
#define B_BUSY		0x0010	/* owned by a thread, in flight */
#define B_DELWRI	0x0020	/* dirty, deferred write-back   */
#define B_VALID		0x0040	/* b_addr reflects on-disk data */
#define B_ASYNC		0x0080	/* don't wait -- iodone brelses */

struct buf {
	dev_t      b_dev;
	uint64_t   b_blkno;
	uint32_t   b_bcount;	/* bytes the driver should transfer */
	uint32_t   b_resid;	/* bytes NOT transferred (error only) */
	uint32_t   b_flags;
	int        b_error;	/* errno-ish code when B_ERROR is set */
	void      *b_addr;	/* read INTO / write FROM this buffer */
	void      *b_data;	/* owned cache page (alias of b_addr
				 * for buffer-cache flow) */
	void     (*b_iodone)(struct buf *);
	void      *b_private;

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

/* Synchronous write: pushes b_addr to the device, marks B_VALID,
 * clears B_DELWRI.  Returns 0 on success, -1 on driver error. */
int  bwrite(struct buf *bp);

/* Delayed write: mark B_DELWRI; bdflush will commit later.  Cheap. */
void bdwrite(struct buf *bp);

/* Optional callback: hint that this buf is unlikely to be reused
 * soon.  Just brelse for now -- the LRU does the work. */
void bawrite(struct buf *bp);

/* Driver-side completion call.  Sets B_DONE (and B_ERROR if the
 * transfer failed -- which the driver signals by storing
 * b_resid > 0 + setting b_error), wakes anyone waiting, and runs
 * b_iodone.  Sync drivers call this from inside d_strategy before
 * returning; async drivers call it from the completion IRQ. */
void biodone(struct buf *bp);

/* Wait until B_DONE is set.  Sync drivers complete inside
 * d_strategy so this is usually a no-op; the call exists for the
 * async path. */
int  biowait(struct buf *bp);

/* Diagnostic: dump cache stats via kprintf. */
void buf_stats(void);

/* In-memory test block device + selftest harness.  bram_init
 * registers BDEV_MAJ_RAM minor 0 backed by a static 256 KiB
 * buffer; buf_selftest exercises the cache through it. */
void bram_init    (void);
void buf_selftest (void);

#endif

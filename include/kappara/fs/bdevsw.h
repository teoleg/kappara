/*
 * include/kappara/fs/bdevsw.h -- SVR4-style block-device switch
 *
 * Parallel to cdevsw.h: the kernel keeps a bdevsw[] indexed by
 * major number; each entry holds the driver's block-I/O entry
 * points.  Block I/O always flows through the buffer cache
 * (kappara/fs/buf.h), so drivers only ever see fully-formed struct
 * buf requests, never raw byte arguments.
 *
 * SVR4 driver shape (which we now follow):
 *
 *   d_open  (minor)               -- driver bring-up if needed
 *   d_close (minor)               -- teardown
 *   d_strategy(minor, struct buf *bp)
 *                                  -- start the I/O described by bp.
 *                                     Sync drivers do the work
 *                                     immediately, set B_DONE +
 *                                     (on error) B_ERROR, and call
 *                                     biodone(bp) before returning.
 *                                     Async drivers enqueue, return,
 *                                     and call biodone() from the
 *                                     completion IRQ.
 *
 * bp tells the driver:
 *   bp->b_blkno   block number on the device
 *   bp->b_bcount  bytes the driver should transfer (multiple of
 *                 the device's logical sector size)
 *   bp->b_flags   B_READ or B_WRITE (direction), plus B_DELWRI etc.
 *   bp->b_addr    where to read INTO or write FROM
 *
 * SVR4 vs Linux:
 *   Linux: gendisk + block_device_operations + bio submission queue.
 *   SVR4:  bdevsw[major] = (open, close, strategy).  strategy(bp)
 *          enqueues the buf on the driver's IO queue; iodone(bp)
 *          fires when the transfer finishes.
 *
 * v2 ships strategy as the only entry point.  Sync drivers
 * complete inline; async drivers can plug iodone in when DMA
 * lands on a real backend.
 */
#ifndef KAPPARA_BDEVSW_H
#define KAPPARA_BDEVSW_H

#include <stdint.h>

#include "kappara/io/cdevsw.h"	/* dev_t / MAJOR / MINOR / MKDEV */

struct buf;	/* fwd; defined in kappara/fs/buf.h */

/* Block-device entry.  Drivers register one of these via
 * bdev_register; the buffer cache calls d_strategy on every miss
 * and every flush. */
struct bdev_entry {
	const char *name;
	int  (*d_open)    (unsigned minor);
	int  (*d_close)   (unsigned minor);
	void (*d_strategy)(unsigned minor, struct buf *bp);
	unsigned block_size;	/* logical sector size; 512 or 4096 */
};

/* Reserved majors -- bumped as new block drivers come up.  Kept
 * disjoint from cdevsw majors so dev_t stays unambiguous: a dev_t
 * tells you whether it's a char or block device by the namespace
 * of its major. */
#define BDEV_MAJ_RAM		1	/* in-memory test disk (bram)     */
#define BDEV_MAJ_MMC		2	/* SD/EMMC (phase S2)             */
#define BDEV_MAJ_RAMDISK	3	/* ramdisk.c -- /usr/bin + /home  */
#define BDEV_MAJ_NVME		4	/* AWS.md stage F -- NVMe ns1     */

#define BDEV_MAX		16

/* Install `e` at bdevsw[major].  Returns 0 on success, -1 if the
 * slot is taken or out of range. */
int                bdev_register(unsigned major, struct bdev_entry *e);
struct bdev_entry *bdev_lookup  (unsigned major);

/* Walk every registered (major, entry) pair.  cb returns non-zero
 * to stop iteration; bdev_for_each returns that value.  Used by
 * future /proc/devices and the /dev mknod helpers. */
int                bdev_for_each(int (*cb)(unsigned major,
				           struct bdev_entry *e,
				           void *arg),
				 void *arg);

#endif

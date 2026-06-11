/*
 * include/kappara/fs/bdevsw.h -- SVR4-style block-device switch
 *
 * Parallel to cdevsw.h: the kernel keeps a bdevsw[] indexed by
 * major number; each entry holds the driver's block-I/O entry
 * points.  Block I/O always flows through the buffer cache
 * (kappara/fs/buf.h), so drivers only ever see fully-formed struct
 * buf requests, never raw bytes.
 *
 * Phase S1.  See docs/ARCHITECTURE.md (eventual block-IO section)
 * for the bigger picture once SD/EMMC support lands in S2.
 *
 * SVR4 vs Linux:
 *   Linux: gendisk + block_device_operations + bio submission queue.
 *   SVR4:  bdevsw[major] = (open, close, strategy).  strategy(bp)
 *          enqueues the buf on the driver's IO queue; iodone(bp)
 *          fires when the transfer finishes.
 *
 * v1 ships strategy as a synchronous read_block / write_block pair
 * because we don't yet have async DMA on any backend; the
 * b_iodone callback is reserved for when DMA arrives.
 */
#ifndef KAPPARA_BDEVSW_H
#define KAPPARA_BDEVSW_H

#include <stdint.h>

#include "kappara/io/cdevsw.h"	/* dev_t / MAJOR / MINOR / MKDEV */

struct buf;	/* fwd; defined in kappara/fs/buf.h */

/* Block-device entry.  Drivers register one of these via
 * bdev_register; the buffer-cache calls into it when a cache miss
 * forces real I/O. */
struct bdev_entry {
	const char *name;
	int (*read_block) (unsigned minor, uint64_t blkno, void *buf);
	int (*write_block)(unsigned minor, uint64_t blkno,
	                   const void *buf);
	unsigned block_size;	/* bytes per block; typically 512 or 4096 */
};

/* Reserved majors -- bumped as new block drivers come up.  Kept
 * disjoint from cdevsw majors so dev_t stays unambiguous: a dev_t
 * tells you whether it's a char or block device by the namespace
 * of its major. */
#define BDEV_MAJ_RAM		1	/* in-memory test disk      */
#define BDEV_MAJ_MMC		2	/* SD/EMMC (phase S2)       */

#define BDEV_MAX		16

/* Install `e` at bdevsw[major].  Returns 0 on success, -1 if the
 * slot is taken or out of range. */
int               bdev_register(unsigned major, struct bdev_entry *e);
struct bdev_entry *bdev_lookup (unsigned major);

#endif

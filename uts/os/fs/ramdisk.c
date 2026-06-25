/*
 * uts/os/fs/ramdisk.c -- in-memory block devices
 * ==============================================
 *
 * Two in-RAM disks behind a single bdevsw[BDEV_MAJ_RAMDISK] entry:
 *
 *   minor 0  ramdisk0 (2048 * 512 = 1 MiB)   <- backs /usr/bin
 *   minor 1  ramdisk1 (1024 * 512 = 512 KiB) <- backs /home (when no NVMe)
 *
 * Both are SVR4-style: the only entry the rest of the kernel can
 * reach is d_strategy(minor, struct buf *), which fulfils the I/O
 * inline and calls biodone() before returning.  Filesystems never
 * touch ramdisk_* directly; they hold a `struct block_device`
 * handle (name + nblocks + dev_t) and go through the buffer cache.
 *
 * Replacing a ramdisk with a real driver later means slotting in
 * a different d_strategy under a different major -- the kfs layer
 * doesn't see the difference.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/fs/bdevsw.h"
#include "kappara/fs/blkdev.h"
#include "kappara/fs/buf.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"

/* RAMDISK0_BLOCKS = 2048 blocks * 512 B = 1 MiB.  Sized so the
 * cmd ELF programs all fit; KFS_BLOCKS_PER_FILE = 64 means the
 * ceiling is (RAMDISK0_BLOCKS - 3) / 64 files. */
#define RAMDISK0_BLOCKS	2048
#define RAMDISK1_BLOCKS	1024	/* /home when no NVMe -- 512 KiB */

#define RAMDISK_MINOR_USRBIN	0
#define RAMDISK_MINOR_HOME	1

static unsigned char ramdisk0_storage[RAMDISK0_BLOCKS * BLK_SIZE];
static unsigned char ramdisk1_storage[RAMDISK1_BLOCKS * BLK_SIZE];

/* d_strategy: pick the storage by minor, copy one sector, biodone.
 * Sync inline -- the SVR4 contract is "complete via biodone before
 * returning OR async via an iodone IRQ"; for an in-memory disk the
 * sync path is the obvious one. */
static void ramdisk_strategy(unsigned minor, struct buf *bp)
{
	unsigned char *base = NULL;
	uint32_t       cap  = 0;

	switch (minor) {
	case RAMDISK_MINOR_USRBIN: base = ramdisk0_storage; cap = RAMDISK0_BLOCKS; break;
	case RAMDISK_MINOR_HOME:   base = ramdisk1_storage; cap = RAMDISK1_BLOCKS; break;
	default:
		bp->b_flags |= B_ERROR;
		bp->b_error = -1;
		bp->b_resid = bp->b_bcount;
		biodone(bp);
		return;
	}

	if (bp->b_blkno >= cap || bp->b_bcount != BLK_SIZE) {
		bp->b_flags |= B_ERROR;
		bp->b_error = -1;
		bp->b_resid = bp->b_bcount;
		biodone(bp);
		return;
	}

	unsigned char *sector = base + (size_t)bp->b_blkno * BLK_SIZE;
	if (bp->b_flags & B_READ)
		kmemcpy(bp->b_addr, sector, BLK_SIZE);
	else
		kmemcpy(sector, bp->b_addr, BLK_SIZE);
	bp->b_resid = 0;
	biodone(bp);
}

static struct bdev_entry ramdisk_entry = {
	.name       = "ramdisk",
	.d_strategy = ramdisk_strategy,
	.block_size = BLK_SIZE,
};

/* Public block_device handles -- (name, nblocks, dev_t).  No
 * function pointers: bread/bwrite + bdevsw[major].d_strategy do
 * all the work. */
static struct block_device ramdisk_bd = {
	.bd_name    = "ramdisk0",
	.bd_nblocks = RAMDISK0_BLOCKS,
	/* bd_dev set at init time so dev_t == MKDEV(...) macro can
	 * expand against the real BDEV_MAJ_RAMDISK constant. */
};
static struct block_device ramdisk_home_bd = {
	.bd_name    = "ramdisk1",
	.bd_nblocks = RAMDISK1_BLOCKS,
};

void ramdisk_init(void)
{
	kmemset(ramdisk0_storage, 0, sizeof(ramdisk0_storage));
	ramdisk_bd.bd_dev = MKDEV(BDEV_MAJ_RAMDISK, RAMDISK_MINOR_USRBIN);
	/* One bdev_register per major -- both minors share strategy. */
	(void)bdev_register(BDEV_MAJ_RAMDISK, &ramdisk_entry);
	kprintf("ramdisk: %u blocks of %u bytes (%u KB)\n",
		(unsigned)RAMDISK0_BLOCKS, (unsigned)BLK_SIZE,
		(unsigned)(RAMDISK0_BLOCKS * BLK_SIZE / 1024));
}

struct block_device *ramdisk_get(void)
{
	return &ramdisk_bd;
}

void ramdisk_home_init(void)
{
	kmemset(ramdisk1_storage, 0, sizeof(ramdisk1_storage));
	ramdisk_home_bd.bd_dev = MKDEV(BDEV_MAJ_RAMDISK, RAMDISK_MINOR_HOME);
	kprintf("ramdisk: home %u blocks of %u bytes (%u KB)\n",
		(unsigned)RAMDISK1_BLOCKS, (unsigned)BLK_SIZE,
		(unsigned)(RAMDISK1_BLOCKS * BLK_SIZE / 1024));
}

struct block_device *ramdisk_home_get(void)
{
	return &ramdisk_home_bd;
}

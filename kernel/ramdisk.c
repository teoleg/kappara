/*
 * kernel/ramdisk.c -- in-memory block device
 * ==========================================
 *
 * The simplest possible struct block_device implementation: a static
 * BSS array of bytes, read/write = kmemcpy in or out.  Non-persistent
 * (contents reset every boot) but exercises the entire filesystem
 * layer cake above it (kfs -> blkdev -> ramdisk).
 *
 * Replacing ramdisk with a real driver later (virtio-blk on QEMU,
 * SDHCI on Pi 3, etc.) means slotting in a different struct
 * block_device behind the same ramdisk_get() call site -- the kfs
 * layer doesn't see the difference.
 *
 *   ramdisk_init     allocate + zero the storage; ready for use
 *   ramdisk_get      returns the singleton struct block_device *
 *
 * Storage size
 * ------------
 * RAMDISK_BLOCKS is the count of 512 B sectors.  64 = 32 KB, which
 * is plenty for our tiny kfs (super + dir + a few small files).
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/blkdev.h"
#include "kappara/printk.h"
#include "kappara/string.h"

#define RAMDISK_BLOCKS	64

static unsigned char ramdisk_storage[RAMDISK_BLOCKS * BLK_SIZE];

static int ramdisk_read(struct block_device *bd,
			uint32_t blkno, void *buf)
{
	(void)bd;
	if (blkno >= RAMDISK_BLOCKS)
		return -1;
	kmemcpy(buf, ramdisk_storage + (size_t)blkno * BLK_SIZE, BLK_SIZE);
	return 0;
}

static int ramdisk_write(struct block_device *bd,
			 uint32_t blkno, const void *buf)
{
	(void)bd;
	if (blkno >= RAMDISK_BLOCKS)
		return -1;
	kmemcpy(ramdisk_storage + (size_t)blkno * BLK_SIZE, buf, BLK_SIZE);
	return 0;
}

static struct block_device ramdisk_bd = {
	.bd_name    = "ramdisk0",
	.bd_nblocks = RAMDISK_BLOCKS,
	.bd_read    = ramdisk_read,
	.bd_write   = ramdisk_write,
};

void ramdisk_init(void)
{
	kmemset(ramdisk_storage, 0, sizeof(ramdisk_storage));
	kprintf("ramdisk: %u blocks of %u bytes (%u KB)\n",
		(unsigned)RAMDISK_BLOCKS, (unsigned)BLK_SIZE,
		(unsigned)(RAMDISK_BLOCKS * BLK_SIZE / 1024));
}

struct block_device *ramdisk_get(void)
{
	return &ramdisk_bd;
}

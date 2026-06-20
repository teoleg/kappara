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
 * RAMDISK_BLOCKS is the count of 512 B sectors.  Sized to fit the
 * cmd ELF programs (each ~12 KB) that R0 boots from kfs instead
 * of in-kernel blob registration.  1024 blocks = 512 KB which is
 * plenty for the 9 programs we ship today plus headroom.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/fs/blkdev.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"

/* RAMDISK_BLOCKS = 2048 blocks * 512 B = 1 MB.  Bumped from 1024
 * once /usr/bin grew past 15 ELFs -- the kfs slot scheme reserves
 * KFS_BLOCKS_PER_FILE = 64 blocks per file regardless of actual size,
 * so the ceiling is (RAMDISK_BLOCKS - 3) / 64 files.  1024 blocks
 * caps at 15; 2048 gets us to 31 with room to spare. */
#define RAMDISK_BLOCKS	2048

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

/* ---- ramdisk1 = /home -------------------------------------------------
 *
 * Second independent ramdisk backing the writable /home mount.  Same
 * shape as ramdisk0; the only difference is its own storage region
 * and bd name so the kfs_mnt_for() lookup in kfs.c picks the right
 * bitmap.  Sized to fit a few uploaded ELFs.
 */
#define RAMDISK_HOME_BLOCKS	1024

static unsigned char ramdisk_home_storage[RAMDISK_HOME_BLOCKS * BLK_SIZE];

static int ramdisk_home_read(struct block_device *bd,
			     uint32_t blkno, void *buf)
{
	(void)bd;
	if (blkno >= RAMDISK_HOME_BLOCKS)
		return -1;
	kmemcpy(buf, ramdisk_home_storage + (size_t)blkno * BLK_SIZE, BLK_SIZE);
	return 0;
}

static int ramdisk_home_write(struct block_device *bd,
			      uint32_t blkno, const void *buf)
{
	(void)bd;
	if (blkno >= RAMDISK_HOME_BLOCKS)
		return -1;
	kmemcpy(ramdisk_home_storage + (size_t)blkno * BLK_SIZE, buf, BLK_SIZE);
	return 0;
}

static struct block_device ramdisk_home_bd = {
	.bd_name    = "ramdisk1",
	.bd_nblocks = RAMDISK_HOME_BLOCKS,
	.bd_read    = ramdisk_home_read,
	.bd_write   = ramdisk_home_write,
};

void ramdisk_home_init(void)
{
	kmemset(ramdisk_home_storage, 0, sizeof(ramdisk_home_storage));
	kprintf("ramdisk: home %u blocks of %u bytes (%u KB)\n",
		(unsigned)RAMDISK_HOME_BLOCKS, (unsigned)BLK_SIZE,
		(unsigned)(RAMDISK_HOME_BLOCKS * BLK_SIZE / 1024));
}

struct block_device *ramdisk_home_get(void)
{
	return &ramdisk_home_bd;
}

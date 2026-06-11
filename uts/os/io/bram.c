/*
 * kernel/io/bram.c -- in-memory block device + S1 buf-cache selftest
 *
 * A 64-block (256 KiB) ramdisk that registers under BDEV_MAJ_RAM.
 * Just enough to exercise the buffer cache end-to-end without
 * waiting for the SD/EMMC driver in S2.  Once that lands, this
 * file becomes the regression-test backbone for "do reads come
 * back the same on cache hit vs miss".
 *
 * Phase S1.
 */

#include <stdint.h>

#include "kappara/fs/bdevsw.h"
#include "kappara/fs/buf.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"

#define BRAM_BLOCKS	64

/* 64 blocks * 4 KiB = 256 KiB.  Fits in BSS without complaint. */
static unsigned char bram_data[BRAM_BLOCKS][BUF_BLOCK_SIZE];

static int bram_read(unsigned minor, uint64_t blkno, void *buf)
{
	(void)minor;
	if (blkno >= BRAM_BLOCKS) return -1;
	kmemcpy(buf, bram_data[blkno], BUF_BLOCK_SIZE);
	return 0;
}

static int bram_write(unsigned minor, uint64_t blkno, const void *buf)
{
	(void)minor;
	if (blkno >= BRAM_BLOCKS) return -1;
	kmemcpy(bram_data[blkno], buf, BUF_BLOCK_SIZE);
	return 0;
}

static struct bdev_entry bram_entry = {
	.name        = "ram",
	.read_block  = bram_read,
	.write_block = bram_write,
	.block_size  = BUF_BLOCK_SIZE,
};

void bram_init(void)
{
	bdev_register(BDEV_MAJ_RAM, &bram_entry);
}

/* ---- S1 selftest ----------------------------------------------------- */

void buf_selftest(void)
{
	int ok = 1;
	dev_t dev = MKDEV(BDEV_MAJ_RAM, 0);

	/* (1) Write block 0, read it back, expect identical bytes. */
	struct buf *bp0 = getblk(dev, 0);
	if (!bp0) {
		kprintf("buf: SELFTEST FAIL getblk(0)\n");
		return;
	}
	for (unsigned i = 0; i < 32; i++)
		((unsigned char *)bp0->b_data)[i] = (unsigned char)(0x42 + i);
	if (bwrite(bp0) < 0) {
		kprintf("buf: SELFTEST FAIL bwrite(0)\n");
		return;
	}

	struct buf *bp0r = bread(dev, 0);
	if (!bp0r) {
		kprintf("buf: SELFTEST FAIL bread(0)\n");
		return;
	}
	for (unsigned i = 0; i < 32; i++) {
		if (((unsigned char *)bp0r->b_data)[i]
		    != (unsigned char)(0x42 + i)) {
			kprintf("buf: SELFTEST FAIL mismatch at %u\n", i);
			ok = 0;
			break;
		}
	}
	brelse(bp0r);

	/* (2) Cache hit: bread(dev,0) twice with no eviction between
	 *     should return the same buf with B_VALID set, and the
	 *     second call shouldn't re-issue a driver read.  We can't
	 *     directly observe the driver call count here, but we can
	 *     check that the first call set B_VALID. */
	struct buf *bp0a = bread(dev, 0);
	struct buf *bp0b;
	if (!bp0a || !(bp0a->b_flags & B_VALID)) {
		kprintf("buf: SELFTEST FAIL valid bit\n");
		ok = 0;
	}
	brelse(bp0a);
	bp0b = bread(dev, 0);
	if (!bp0b || bp0b != bp0a) {
		/* Same slot still cached -- LRU hasn't evicted us. */
		kprintf("buf: SELFTEST WARN cache evicted (bp0a=%p bp0b=%p)\n",
		        (void *)bp0a, (void *)bp0b);
	}
	if (bp0b) brelse(bp0b);

	/* (3) Eviction: walk past BUF_POOL_SIZE distinct blocks, then
	 *     bread(0) again -- should still return correct data. */
	for (unsigned i = 1; i < BUF_POOL_SIZE + 4; i++) {
		struct buf *bp = bread(dev, i);
		if (bp) brelse(bp);
	}
	struct buf *bp0c = bread(dev, 0);
	if (!bp0c) {
		kprintf("buf: SELFTEST FAIL re-read after eviction\n");
		ok = 0;
	} else {
		for (unsigned i = 0; i < 32; i++) {
			if (((unsigned char *)bp0c->b_data)[i]
			    != (unsigned char)(0x42 + i)) {
				kprintf("buf: SELFTEST FAIL post-evict mismatch %u\n", i);
				ok = 0;
				break;
			}
		}
		brelse(bp0c);
	}

	/* (4) Delayed write + flush via bwrite path. */
	struct buf *bp1 = getblk(dev, 1);
	((unsigned char *)bp1->b_data)[0] = 0xab;
	bdwrite(bp1);
	struct buf *bp1r = bread(dev, 1);
	if (!bp1r) {
		kprintf("buf: SELFTEST FAIL bread after bdwrite\n");
		ok = 0;
	} else {
		if (((unsigned char *)bp1r->b_data)[0] != 0xab) {
			kprintf("buf: SELFTEST FAIL bdwrite content\n");
			ok = 0;
		}
		brelse(bp1r);
	}

	if (ok) kprintf("buf: selftest PASS\n");
}

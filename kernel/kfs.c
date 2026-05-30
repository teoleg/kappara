/*
 * kernel/kfs.c -- read-only mount for the v1 kappara filesystem
 * =============================================================
 *
 * See include/kappara/kfs.h for the on-disk layout.  This file:
 *
 *   1. kfs_mkimage(bd)   formats `bd` with a known set of files
 *                        (built-in payloads since we have no `mkfs`).
 *   2. kfs_mount(bd, dp) reads the superblock + directory off the
 *                        device and creates a regfile inode under
 *                        the VFS directory `dp` for each entry.
 *   3. regfile_fops       file_ops for those inodes: open allocates
 *                        a position cursor; read pulls one block at
 *                        a time via bd->bd_read until the offset
 *                        reaches the file's size.
 *
 * Limitations of this first cut
 * -----------------------------
 *   - read-only.  No write/truncate/append yet.
 *   - one read per call returns up to (BLK_SIZE - offset_in_block)
 *     bytes; user/init.c's read loop with chunked sys_read drains
 *     the whole file across multiple syscalls.
 *   - flat namespace per kfs mount (no subdirs within kfs).
 *   - no buffer cache; every read goes straight to the block device.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/blkdev.h"
#include "kappara/kfs.h"
#include "kappara/kmem.h"
#include "kappara/printk.h"
#include "kappara/string.h"
#include "kappara/vfs.h"

/* ---- regular-file file_ops ----------------------------------------- */

struct regfile_cursor {
	struct kfs_file	*kf;
	uint32_t	 pos;
};

static int regfile_open(struct file *f)
{
	struct regfile_cursor *c = kmalloc(sizeof(*c));
	if (!c)
		return -1;
	c->kf  = (struct kfs_file *)f->f_inode->i_private;
	c->pos = 0;
	f->f_private = c;
	return 0;
}

static int regfile_close(struct file *f)
{
	if (f->f_private)
		kfree(f->f_private);
	f->f_private = NULL;
	return 0;
}

static long regfile_read(struct file *f, void *buf, size_t len)
{
	struct regfile_cursor *c  = f->f_private;
	struct kfs_file       *kf = c->kf;
	if (c->pos >= kf->size_bytes)
		return 0;	/* EOF */

	uint32_t blk    = kf->start_block + (c->pos / BLK_SIZE);
	uint32_t offblk = c->pos % BLK_SIZE;

	unsigned char tmp[BLK_SIZE];
	if (kf->bd->bd_read(kf->bd, blk, tmp) < 0)
		return -1;

	size_t avail   = BLK_SIZE - offblk;
	size_t file_av = kf->size_bytes - c->pos;
	size_t n       = len;
	if (n > avail)   n = avail;
	if (n > file_av) n = file_av;

	kmemcpy(buf, tmp + offblk, n);
	c->pos += (uint32_t)n;
	return (long)n;
}

/* No write / ioctl / putmsg / getmsg for kfs files yet -- regular
 * files are just bytes, no STREAMS plumbing. */
static struct file_ops regfile_fops = {
	.open  = regfile_open,
	.close = regfile_close,
	.read  = regfile_read,
};

/* ---- mkimage: build a fresh fs in `bd` from baked-in files --------- */

struct payload {
	const char	*name;
	const char	*data;
	uint32_t	 size;
};

#define PL(name, str)	{ name, str, (uint32_t)(sizeof(str) - 1) }

static const struct payload payloads[] = {
	PL("hello.txt", "hello, kfs!\nthis came off block 2 of the ramdisk.\n"),
	PL("readme",    "kappara filesystem v1\nflat directory, read-only, "
			"backed by a ramdisk that resets every boot.\n"),
	PL("motd",      "no soup for you, only streams.\n"),
};

#define NPAYLOADS (sizeof(payloads) / sizeof(payloads[0]))

void kfs_mkimage(struct block_device *bd)
{
	/* Superblock at block 0. */
	struct kfs_super sb;
	kmemset(&sb, 0, sizeof(sb));
	sb.magic     = KFS_MAGIC;
	sb.num_files = NPAYLOADS;
	bd->bd_write(bd, 0, &sb);

	/* Directory at block 1; up to KFS_DIRENTS entries. */
	struct kfs_dirent dir[KFS_DIRENTS];
	kmemset(dir, 0, sizeof(dir));

	uint32_t cur_block = 2;
	for (unsigned i = 0; i < NPAYLOADS && i < KFS_DIRENTS; i++) {
		const struct payload *p = &payloads[i];
		size_t nlen = 0;
		while (p->name[nlen] && nlen < KFS_NAME_MAX - 1) {
			dir[i].name[nlen] = p->name[nlen];
			nlen++;
		}
		dir[i].name[nlen]    = '\0';
		dir[i].start_block   = cur_block;
		dir[i].size_bytes    = p->size;

		/* Write the file data, one block at a time. */
		const char *src = p->data;
		uint32_t    rem = p->size;
		uint32_t    blk = cur_block;
		while (rem > 0) {
			unsigned char buf[BLK_SIZE];
			kmemset(buf, 0, sizeof(buf));
			size_t n = (rem < BLK_SIZE) ? rem : BLK_SIZE;
			kmemcpy(buf, src, n);
			bd->bd_write(bd, blk, buf);
			blk++;
			src += n;
			rem -= n;
		}
		cur_block = blk;
	}
	bd->bd_write(bd, 1, dir);

	kprintf("kfs: mkimage wrote %u files (blocks used: 0..%u)\n",
		(unsigned)NPAYLOADS, (unsigned)(cur_block - 1));
}

/* ---- mount: pull metadata off the device, attach to the VFS -------- */

int kfs_mount(struct block_device *bd, struct dentry *mountpoint)
{
	struct kfs_super sb;
	if (bd->bd_read(bd, 0, &sb) < 0) {
		kprintf("kfs_mount: superblock read failed\n");
		return -1;
	}
	if (sb.magic != KFS_MAGIC) {
		kprintf("kfs_mount: bad magic 0x%lx (want 0x%lx)\n",
			(unsigned long)sb.magic, (unsigned long)KFS_MAGIC);
		return -1;
	}

	struct kfs_dirent dir[KFS_DIRENTS];
	if (bd->bd_read(bd, 1, dir) < 0) {
		kprintf("kfs_mount: directory read failed\n");
		return -1;
	}

	unsigned n = sb.num_files;
	if (n > KFS_DIRENTS) n = KFS_DIRENTS;

	for (unsigned i = 0; i < n; i++) {
		struct kfs_file *kf = kmalloc(sizeof(*kf));
		if (!kf) continue;
		kf->bd          = bd;
		kf->start_block = dir[i].start_block;
		kf->size_bytes  = dir[i].size_bytes;

		/* dir[i].name is in our stack-local buffer; the dentry
		 * stores d_name as a raw pointer (callers like vfs_mkdir
		 * usually pass string literals).  Make a kmalloc copy
		 * with stable lifetime. */
		char *nm = kmalloc(KFS_NAME_MAX);
		if (!nm) { kfree(kf); continue; }
		for (int j = 0; j < KFS_NAME_MAX; j++)
			nm[j] = dir[i].name[j];
		nm[KFS_NAME_MAX - 1] = '\0';

		vfs_mknod_regfile(mountpoint, nm, &regfile_fops, kf);
	}

	kprintf("kfs: mounted '%s' on /%s (%u files)\n",
		bd->bd_name, mountpoint->d_name, n);
	return 0;
}

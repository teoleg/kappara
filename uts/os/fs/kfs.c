/*
 * kernel/kfs.c -- mount / mkimage for the v2 kappara filesystem
 * =============================================================
 *
 * See include/kappara/fs/kfs.h for the on-disk layout.  This file:
 *
 *   1. kfs_mkimage(bd)   formats `bd`: superblock at block 0, the
 *                        free-block bitmap at block 1, the root
 *                        dirent table at block 2, then a fixed
 *                        KFS_BLOCKS_PER_FILE-block run per built-in
 *                        payload.  Every allocated block is marked
 *                        used in the bitmap before it is flushed.
 *   2. kfs_mount(bd, dp) reads superblock + bitmap off the device,
 *                        walks the root dirent table, and creates
 *                        an inode under the VFS directory `dp` for
 *                        each entry (recursively for subdirs).
 *   3. regfile_fops       file_ops for files: open/close manage a
 *                        position cursor; read/write/seek move
 *                        bytes one block at a time via bd_read /
 *                        bd_write; O_TRUNC zeroes size on open.
 *   4. kfs_dir_fops       file_ops for directories: creat / mkdir /
 *                        unlink / rmdir allocate or free runs via
 *                        kfs_alloc_block_zero / kfs_free_blocks and
 *                        update the parent dirent table.
 *
 * v2 vs v1
 * --------
 * v1's "allocator" was a monotonic counter (next_free_block) in the
 * superblock.  Deleted files leaked their blocks forever -- rm
 * cleared the dirent but never reclaimed storage.  v2 swaps that
 * counter for a real bitmap so kfs_alloc_block_zero can find any
 * contiguous run of free blocks and rm / rmdir can return them to
 * the pool.
 *
 * Limitations still here
 * ----------------------
 *   - per-file allocation is fixed at mkimage / creat time
 *     (KFS_BLOCKS_PER_FILE blocks).  Append past that returns EIO.
 *   - no buffer cache: every read/write hits the block device.
 *   - bitmap holds only the first 4096 blocks (one 512 B page).
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/fs/blkdev.h"
#include "kappara/fs/kfs.h"
#include "kappara/core/kmem.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"
#include "kappara/fs/vfs.h"

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

	/* O_TRUNC: zero size + push to disk so subsequent reads see EOF
	 * immediately and `echo` style overwrites don't leak the tail of
	 * the previous content. */
	if (f->f_flags & O_TRUNC) {
		struct kfs_file *kf = c->kf;
		kf->size_bytes = 0;
		struct kfs_dirent dir[KFS_DIRENTS];
		if (kf->bd->bd_read(kf->bd, kf->dir_block, dir) == 0) {
			dir[kf->dirent_idx].size_bytes = 0;
			kf->bd->bd_write(kf->bd, kf->dir_block, dir);
		}
	}
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

/*
 * Write bytes at the cursor.  Bounded by the file's pre-allocated
 * KFS_BLOCKS_PER_FILE blocks; we never re-allocate.  If the cursor
 * advances past the previous EOF we update size_bytes in memory and
 * push the change back to the directory block on the device so it
 * survives across mount cycles (once we have persistent storage).
 *
 * Each block is read-modify-written so we preserve neighbouring
 * bytes when len doesn't fill the whole block.
 */
static long regfile_write(struct file *f, const void *buf, size_t len)
{
	struct regfile_cursor *c  = f->f_private;
	struct kfs_file       *kf = c->kf;

	uint32_t max = kf->alloc_blocks * BLK_SIZE;
	if (c->pos >= max)
		return -1;
	if (c->pos + len > max)
		len = max - c->pos;

	const unsigned char *src = buf;
	size_t written = 0;
	while (written < len) {
		uint32_t off_total = c->pos + (uint32_t)written;
		uint32_t blk       = kf->start_block + off_total / BLK_SIZE;
		uint32_t offblk    = off_total % BLK_SIZE;

		unsigned char tmp[BLK_SIZE];
		if (kf->bd->bd_read(kf->bd, blk, tmp) < 0)
			break;

		size_t n = BLK_SIZE - offblk;
		if (n > len - written)
			n = len - written;
		kmemcpy(tmp + offblk, src + written, n);

		if (kf->bd->bd_write(kf->bd, blk, tmp) < 0)
			break;
		written += n;
	}
	c->pos += (uint32_t)written;

	if (c->pos > kf->size_bytes) {
		kf->size_bytes = c->pos;
		/* Push the new size back to the directory entry on disk. */
		struct kfs_dirent dir[KFS_DIRENTS];
		if (kf->bd->bd_read(kf->bd, kf->dir_block, dir) == 0) {
			dir[kf->dirent_idx].size_bytes = kf->size_bytes;
			kf->bd->bd_write(kf->bd, kf->dir_block, dir);
		}
	}
	return (long)written;
}

static long regfile_seek(struct file *f, long offset, int whence)
{
	struct regfile_cursor *c  = f->f_private;
	struct kfs_file       *kf = c->kf;
	long newpos;
	switch (whence) {
	case 0: newpos = offset; break;			/* SEEK_SET */
	case 1: newpos = (long)c->pos + offset; break;	/* SEEK_CUR */
	case 2: newpos = (long)kf->size_bytes + offset; break;	/* SEEK_END */
	default: return -1;
	}
	if (newpos < 0)
		return -1;
	c->pos = (uint32_t)newpos;
	return newpos;
}

/* Return the file's current size for ls -l / stat.  The kfs_file
 * pointer lives in inode->i_private; it tracks size as bytes are
 * written and as O_TRUNC zeroes the file. */
static long regfile_size(struct inode *ino)
{
	struct kfs_file *kf = (struct kfs_file *)ino->i_private;
	return kf ? (long)kf->size_bytes : -1;
}

/* SVR4 vop_inactive: the inode's last reference just dropped, so
 * the kfs_file metadata behind i_private can go.  The on-disk
 * blocks were already returned to the bitmap by kfs_dir_unlink
 * before vfs_remove_child triggered this. */
static void regfile_inactive(struct inode *ino)
{
	kfree(ino->i_private);
	ino->i_private = NULL;
}

static struct file_ops regfile_fops = {
	.open     = regfile_open,
	.close    = regfile_close,
	.read     = regfile_read,
	.write    = regfile_write,
	.seek     = regfile_seek,
	.size     = regfile_size,
	.inactive = regfile_inactive,
};

/*
 * Per-mount state.  Each kfs_mount(bd, dp) call grabs the first free
 * slot here; bitmap caches stay independent so multiple kfs ramdisks
 * (e.g. /usr/bin + /home) can be mounted side by side without one
 * stomping the other's free-block view.
 *
 * MAX_KFS_MOUNTS sets a static cap; one slot per concurrent kfs.
 * Increase if you ever mount more.
 */
#define MAX_KFS_MOUNTS	4

struct kfs_mnt {
	struct block_device *bd;	/* NULL = slot free */
	uint8_t              bitmap[BLK_SIZE];
};

static struct kfs_mnt kfs_mnts[MAX_KFS_MOUNTS];

/* O(N) by bd pointer.  Called on the hot path of every block alloc /
 * free; with N <= MAX_KFS_MOUNTS this is fine. */
static struct kfs_mnt *kfs_mnt_for(struct block_device *bd)
{
	for (unsigned i = 0; i < MAX_KFS_MOUNTS; i++)
		if (kfs_mnts[i].bd == bd) return &kfs_mnts[i];
	return NULL;
}

static struct kfs_mnt *kfs_mnt_alloc(struct block_device *bd)
{
	for (unsigned i = 0; i < MAX_KFS_MOUNTS; i++) {
		if (kfs_mnts[i].bd == NULL) {
			kfs_mnts[i].bd = bd;
			kmemset(kfs_mnts[i].bitmap, 0, BLK_SIZE);
			return &kfs_mnts[i];
		}
	}
	return NULL;
}

static struct file_ops kfs_dir_fops;	/* fwd; defined below */

static int  kfs_bit_get(struct kfs_mnt *m, uint32_t blk)
{
	return (m->bitmap[blk / 8] >> (blk % 8)) & 1;
}

static void kfs_bit_set(struct kfs_mnt *m, uint32_t blk, int val)
{
	if (val)
		m->bitmap[blk / 8] |=  (uint8_t)(1u << (blk % 8));
	else
		m->bitmap[blk / 8] &= (uint8_t)~(1u << (blk % 8));
}

static void kfs_bitmap_save(struct kfs_mnt *m)
{
	if (m && m->bd)
		m->bd->bd_write(m->bd, KFS_BITMAP_BLOCK, m->bitmap);
}

/*
 * Find the first run of `count` contiguous free blocks and mark them
 * allocated.  Returns the starting block number, or 0 (which is the
 * superblock so never a valid data block) on failure.
 *
 * Also zeroes the freshly-allocated blocks so they read as 0 if
 * something pulls them up before they're written.
 */
static uint32_t kfs_alloc_block_zero(struct block_device *bd, uint32_t count)
{
	struct kfs_mnt *m = kfs_mnt_for(bd);
	if (!m) { kprintf("kfs_alloc: no mnt for bd %p\n", bd); return 0; }

	uint32_t total = bd->bd_nblocks;
	if (total > BLK_SIZE * 8) total = BLK_SIZE * 8;	/* bitmap cap */

	for (uint32_t start = KFS_ROOT_BLOCK + 1;
	     start + count <= total; start++) {
		unsigned ok = 1;
		for (uint32_t i = 0; i < count; i++) {
			if (kfs_bit_get(m, start + i)) { ok = 0; break; }
		}
		if (!ok) continue;

		for (uint32_t i = 0; i < count; i++)
			kfs_bit_set(m, start + i, 1);
		kfs_bitmap_save(m);

		unsigned char zero[BLK_SIZE];
		for (size_t i = 0; i < BLK_SIZE; i++) zero[i] = 0;
		for (uint32_t b = 0; b < count; b++)
			if (bd->bd_write(bd, start + b, zero) < 0) return 0;
		return start;
	}
	kprintf("kfs_alloc: no contiguous %u-block run available\n",
		(unsigned)count);
	return 0;
}

static void kfs_free_blocks(struct block_device *bd,
			    uint32_t start, uint32_t count)
{
	struct kfs_mnt *m = kfs_mnt_for(bd);
	if (!m) return;
	for (uint32_t i = 0; i < count; i++)
		kfs_bit_set(m, start + i, 0);
	kfs_bitmap_save(m);
}

/* Look up the dir_block to operate on for a directory inode.  The
 * inode's i_private points at a struct kfs_dir we set up at mount
 * (or kfs_dir_mkdir) time. */
static struct kfs_dir *kfs_dir_of(struct inode *dir)
{
	return (struct kfs_dir *)dir->i_private;
}

/* Scan a dirent block for a free slot and reject duplicates.
 * Returns the slot index or -1.  Caller already loaded dir_blk. */
static int kfs_dir_find_slot(struct kfs_dirent *dir_blk, const char *name)
{
	int slot = -1;
	size_t nlen = 0;
	while (name[nlen]) nlen++;
	if (nlen >= KFS_NAME_MAX) return -1;
	for (unsigned i = 0; i < KFS_DIRENTS; i++) {
		if (dir_blk[i].name[0] == '\0') {
			if (slot < 0) slot = (int)i;
			continue;
		}
		size_t k;
		for (k = 0; k < KFS_NAME_MAX && dir_blk[i].name[k] == name[k]; k++)
			if (name[k] == '\0') break;
		if (k < KFS_NAME_MAX && dir_blk[i].name[k] == name[k])
			return -1;	/* duplicate */
	}
	return slot;
}

static char *kfs_dup_name(const char *name)
{
	char *nm = kmalloc(KFS_NAME_MAX);
	if (!nm) return NULL;
	size_t i = 0;
	for (; i < KFS_NAME_MAX - 1 && name[i]; i++) nm[i] = name[i];
	nm[i] = '\0';
	return nm;
}

static int kfs_dir_creat(struct inode *dir, const char *name)
{
	struct kfs_dir *d = kfs_dir_of(dir);
	if (!d) return -1;
	struct block_device *bd = d->bd;

	struct kfs_dirent dir_blk[KFS_DIRENTS];
	if (bd->bd_read(bd, d->dir_block, dir_blk) < 0) return -1;

	int slot = kfs_dir_find_slot(dir_blk, name);
	if (slot < 0) { kprintf("kfs_creat: full or dup\n"); return -1; }

	uint32_t start = kfs_alloc_block_zero(bd, KFS_BLOCKS_PER_FILE);
	if (!start) return -1;

	for (size_t i = 0; i < KFS_NAME_MAX; i++) dir_blk[slot].name[i] = 0;
	for (size_t i = 0; name[i] && i < KFS_NAME_MAX - 1; i++)
		dir_blk[slot].name[i] = name[i];
	dir_blk[slot].start_block = start;
	dir_blk[slot].size_bytes  = 0;
	dir_blk[slot].type        = KFS_TYPE_FILE;
	if (bd->bd_write(bd, d->dir_block, dir_blk) < 0) return -1;

	struct kfs_file *kf = kmalloc(sizeof(*kf));
	char *nm = kfs_dup_name(name);
	if (!kf || !nm) return -1;
	kf->bd           = bd;
	kf->start_block  = start;
	kf->size_bytes   = 0;
	kf->alloc_blocks = KFS_BLOCKS_PER_FILE;
	kf->dir_block    = d->dir_block;
	kf->dirent_idx   = (uint8_t)slot;
	vfs_mknod_regfile(d->dentry, nm, &regfile_fops, kf);

	kprintf("kfs_creat: '%s' in dir_block=%u slot=%d blocks=%u..%u\n",
		name, d->dir_block, slot, start,
		start + KFS_BLOCKS_PER_FILE - 1);
	return 0;
}

static int kfs_dir_mkdir(struct inode *dir, const char *name)
{
	struct kfs_dir *d = kfs_dir_of(dir);
	if (!d) return -1;
	struct block_device *bd = d->bd;

	struct kfs_dirent dir_blk[KFS_DIRENTS];
	if (bd->bd_read(bd, d->dir_block, dir_blk) < 0) return -1;

	int slot = kfs_dir_find_slot(dir_blk, name);
	if (slot < 0) { kprintf("kfs_mkdir: full or dup\n"); return -1; }

	uint32_t subblk = kfs_alloc_block_zero(bd, 1);
	if (!subblk) return -1;

	for (size_t i = 0; i < KFS_NAME_MAX; i++) dir_blk[slot].name[i] = 0;
	for (size_t i = 0; name[i] && i < KFS_NAME_MAX - 1; i++)
		dir_blk[slot].name[i] = name[i];
	dir_blk[slot].start_block = subblk;
	dir_blk[slot].size_bytes  = 0;
	dir_blk[slot].type        = KFS_TYPE_DIR;
	if (bd->bd_write(bd, d->dir_block, dir_blk) < 0) return -1;

	/* Hook into VFS: a new directory dentry whose i_fops route
	 * subsequent creat/mkdir back here and whose i_private knows
	 * the new dirent block. */
	char *nm = kfs_dup_name(name);
	struct kfs_dir *sub = kmalloc(sizeof(*sub));
	if (!nm || !sub) return -1;
	struct dentry *sub_dentry = vfs_mkdir(d->dentry, nm);
	if (!sub_dentry) return -1;
	sub->bd        = bd;
	sub->dir_block = subblk;
	sub->dentry    = sub_dentry;
	sub_dentry->d_inode->i_fops    = &kfs_dir_fops;
	sub_dentry->d_inode->i_private = sub;

	kprintf("kfs_mkdir: '%s' dirent_block=%u under dir_block=%u slot=%d\n",
		name, subblk, d->dir_block, slot);
	return 0;
}

/* Scan a directory block for an entry matching `name`.  Returns the
 * slot index or -1.  Compares up to the first NUL on either side. */
static int kfs_dir_find_named(const struct kfs_dirent *dir_blk,
			      const char *name)
{
	for (unsigned i = 0; i < KFS_DIRENTS; i++) {
		if (dir_blk[i].name[0] == '\0')
			continue;
		size_t k = 0;
		while (k < KFS_NAME_MAX
		       && dir_blk[i].name[k] == name[k]
		       && name[k]
		       && dir_blk[i].name[k])
			k++;
		if (k < KFS_NAME_MAX
		    && dir_blk[i].name[k] == '\0'
		    && name[k] == '\0')
			return (int)i;
	}
	return -1;
}

static int kfs_dir_unlink(struct inode *dir, const char *name)
{
	struct kfs_dir *d = kfs_dir_of(dir);
	if (!d) return -1;
	struct block_device *bd = d->bd;

	struct kfs_dirent dir_blk[KFS_DIRENTS];
	if (bd->bd_read(bd, d->dir_block, dir_blk) < 0) return -1;
	int slot = kfs_dir_find_named(dir_blk, name);
	if (slot < 0) {
		kprintf("kfs_unlink: '%s' not found in dir_block=%u\n",
			name, d->dir_block);
		return -1;
	}
	if (dir_blk[slot].type != KFS_TYPE_FILE) {
		kprintf("kfs_unlink: '%s' is not a regular file\n", name);
		return -1;
	}
	/* Reclaim the file's data blocks via the bitmap, then clear the
	 * dirent (name[0]=0 marks empty). */
	uint32_t reclaim_start = dir_blk[slot].start_block;
	for (size_t i = 0; i < KFS_NAME_MAX; i++) dir_blk[slot].name[i] = 0;
	dir_blk[slot].start_block = 0;
	dir_blk[slot].size_bytes  = 0;
	dir_blk[slot].type        = 0;
	if (bd->bd_write(bd, d->dir_block, dir_blk) < 0) return -1;
	if (reclaim_start) kfs_free_blocks(bd, reclaim_start, KFS_BLOCKS_PER_FILE);

	kprintf("kfs_unlink: '%s' from dir_block=%u slot=%d (freed blocks %u..%u)\n",
		name, d->dir_block, slot, reclaim_start,
		reclaim_start + KFS_BLOCKS_PER_FILE - 1);
	return 0;
}

static int kfs_dir_rmdir(struct inode *dir, const char *name)
{
	struct kfs_dir *d = kfs_dir_of(dir);
	if (!d) return -1;
	struct block_device *bd = d->bd;

	struct kfs_dirent dir_blk[KFS_DIRENTS];
	if (bd->bd_read(bd, d->dir_block, dir_blk) < 0) return -1;
	int slot = kfs_dir_find_named(dir_blk, name);
	if (slot < 0) {
		kprintf("kfs_rmdir: '%s' not found\n", name);
		return -1;
	}
	if (dir_blk[slot].type != KFS_TYPE_DIR) {
		kprintf("kfs_rmdir: '%s' is not a directory\n", name);
		return -1;
	}
	/* Refuse to remove a non-empty dir. */
	struct kfs_dirent sub[KFS_DIRENTS];
	uint32_t sub_block = dir_blk[slot].start_block;
	if (bd->bd_read(bd, sub_block, sub) < 0) return -1;
	for (unsigned i = 0; i < KFS_DIRENTS; i++) {
		if (sub[i].name[0] != '\0') {
			kprintf("kfs_rmdir: '%s' is not empty\n", name);
			return -1;
		}
	}

	for (size_t i = 0; i < KFS_NAME_MAX; i++) dir_blk[slot].name[i] = 0;
	dir_blk[slot].start_block = 0;
	dir_blk[slot].size_bytes  = 0;
	dir_blk[slot].type        = 0;
	if (bd->bd_write(bd, d->dir_block, dir_blk) < 0) return -1;
	kfs_free_blocks(bd, sub_block, 1);

	kprintf("kfs_rmdir: '%s' from dir_block=%u slot=%d (freed block %u)\n",
		name, d->dir_block, slot, sub_block);
	return 0;
}

/* Same as regfile_inactive but for the kfs_dir handle that
 * directory inodes hang off of.  Triggered by rmdir + the last
 * close of any open fd referring to this dir. */
static void kfs_dir_inactive(struct inode *ino)
{
	kfree(ino->i_private);
	ino->i_private = NULL;
}

static struct file_ops kfs_dir_fops = {
	.creat    = kfs_dir_creat,
	.mkdir    = kfs_dir_mkdir,
	.unlink   = kfs_dir_unlink,
	.rmdir    = kfs_dir_rmdir,
	.inactive = kfs_dir_inactive,
};

/* ---- mkimage: build a fresh fs in `bd` from the caller's payload table --- */

void kfs_mkimage(struct block_device *bd,
		 const struct kfs_payload *payloads, unsigned n_payloads)
{
	/* Allocate or reuse a kfs_mnt slot for this bd so the bitmap we
	 * compute here is the same slot kfs_mount will reload from disk
	 * later.  Idempotent: mkimage-then-mount on the same bd hits the
	 * same slot. */
	struct kfs_mnt *m = kfs_mnt_for(bd);
	if (!m) m = kfs_mnt_alloc(bd);
	if (!m) {
		kprintf("kfs_mkimage: MAX_KFS_MOUNTS exceeded\n");
		return;
	}
	kmemset(m->bitmap, 0, BLK_SIZE);
	/* Reserve super, bitmap, root dir. */
	kfs_bit_set(m, KFS_SUPER_BLOCK,  1);
	kfs_bit_set(m, KFS_BITMAP_BLOCK, 1);
	kfs_bit_set(m, KFS_ROOT_BLOCK,   1);

	struct kfs_super sb;
	kmemset(&sb, 0, sizeof(sb));
	sb.magic     = KFS_MAGIC;
	sb.num_files = n_payloads;
	bd->bd_write(bd, KFS_SUPER_BLOCK, &sb);

	struct kfs_dirent dir[KFS_DIRENTS];
	kmemset(dir, 0, sizeof(dir));

	/*
	 * Each file gets a fixed KFS_BLOCKS_PER_FILE-block slot so it
	 * can be overwritten or extended in place up to that size, with
	 * no indirect-block management needed.  Unused trailing blocks
	 * are zeroed so they read as zeros if the file later grows into
	 * them.
	 */
	uint32_t cur_block = KFS_ROOT_BLOCK + 1;
	for (unsigned i = 0; i < n_payloads && i < KFS_DIRENTS; i++) {
		const struct kfs_payload *p = &payloads[i];
		size_t nlen = 0;
		while (p->name[nlen] && nlen < KFS_NAME_MAX - 1) {
			dir[i].name[nlen] = p->name[nlen];
			nlen++;
		}
		dir[i].name[nlen]    = '\0';
		dir[i].start_block   = cur_block;
		dir[i].size_bytes    = p->size;
		dir[i].type          = KFS_TYPE_FILE;

		const unsigned char *src = (const unsigned char *)p->data;
		uint32_t             rem = p->size;
		for (uint32_t b = 0; b < KFS_BLOCKS_PER_FILE; b++) {
			unsigned char buf[BLK_SIZE];
			kmemset(buf, 0, sizeof(buf));
			if (rem > 0) {
				size_t n = (rem < BLK_SIZE) ? rem : BLK_SIZE;
				kmemcpy(buf, src, n);
				src += n;
				rem -= n;
			}
			bd->bd_write(bd, cur_block + b, buf);
			kfs_bit_set(m, cur_block + b, 1);
		}
		cur_block += KFS_BLOCKS_PER_FILE;
	}
	bd->bd_write(bd, KFS_ROOT_BLOCK, dir);

	/* Flush the bitmap. */
	bd->bd_write(bd, KFS_BITMAP_BLOCK, m->bitmap);

	kprintf("kfs: mkimage wrote %u files (blocks used: 0..%u)\n",
		n_payloads, (unsigned)(cur_block - 1));
}

/* ---- mount: pull metadata off the device, attach to the VFS -------- */

/* Recursively walk a dir block and populate the VFS subtree rooted
 * at vfs_parent.  Subdirectories get their own dir block + kfs_dir
 * metadata so creat/mkdir on them lands in the right place. */
static void kfs_mount_dir(struct block_device *bd,
			  uint32_t dir_block,
			  struct dentry *vfs_parent)
{
	struct kfs_dirent dir[KFS_DIRENTS];
	if (bd->bd_read(bd, dir_block, dir) < 0)
		return;

	for (unsigned i = 0; i < KFS_DIRENTS; i++) {
		if (dir[i].name[0] == '\0')
			continue;

		char *nm = kfs_dup_name(dir[i].name);
		if (!nm) continue;

		if (dir[i].type == KFS_TYPE_DIR) {
			struct kfs_dir *sub = kmalloc(sizeof(*sub));
			if (!sub) { kfree(nm); continue; }
			struct dentry *sd = vfs_mkdir(vfs_parent, nm);
			if (!sd) { kfree(nm); kfree(sub); continue; }
			sub->bd        = bd;
			sub->dir_block = dir[i].start_block;
			sub->dentry    = sd;
			sd->d_inode->i_fops    = &kfs_dir_fops;
			sd->d_inode->i_private = sub;
			kfs_mount_dir(bd, dir[i].start_block, sd);
		} else {
			struct kfs_file *kf = kmalloc(sizeof(*kf));
			if (!kf) { kfree(nm); continue; }
			kf->bd           = bd;
			kf->start_block  = dir[i].start_block;
			kf->size_bytes   = dir[i].size_bytes;
			kf->alloc_blocks = KFS_BLOCKS_PER_FILE;
			kf->dir_block    = dir_block;
			kf->dirent_idx   = (uint8_t)i;
			vfs_mknod_regfile(vfs_parent, nm,
					  &regfile_fops, kf);
		}
	}
}

int kfs_mount(struct block_device *bd, struct dentry *mountpoint)
{
	struct kfs_super sb;
	if (bd->bd_read(bd, KFS_SUPER_BLOCK, &sb) < 0) {
		kprintf("kfs_mount: superblock read failed\n");
		return -1;
	}
	if (sb.magic != KFS_MAGIC) {
		kprintf("kfs_mount: bad magic 0x%lx (want 0x%lx)\n",
			(unsigned long)sb.magic, (unsigned long)KFS_MAGIC);
		return -1;
	}

	struct kfs_mnt *m = kfs_mnt_for(bd);
	if (!m) m = kfs_mnt_alloc(bd);
	if (!m) {
		kprintf("kfs_mount: MAX_KFS_MOUNTS exceeded\n");
		return -1;
	}
	if (bd->bd_read(bd, KFS_BITMAP_BLOCK, m->bitmap) < 0) {
		kprintf("kfs_mount: bitmap read failed\n");
		return -1;
	}

	/* Publish creat/mkdir on the mountpoint's inode and stash a
	 * kfs_dir so kfs_dir_creat / kfs_dir_mkdir know which dir
	 * block to operate on. */
	struct kfs_dir *root = kmalloc(sizeof(*root));
	if (!root) return -1;
	root->bd        = bd;
	root->dir_block = KFS_ROOT_BLOCK;
	root->dentry    = mountpoint;
	mountpoint->d_inode->i_fops    = &kfs_dir_fops;
	mountpoint->d_inode->i_private = root;

	kfs_mount_dir(bd, KFS_ROOT_BLOCK, mountpoint);

	kprintf("kfs: mounted '%s' at mountpoint '%s'\n",
		bd->bd_name, mountpoint->d_name);
	return 0;
}

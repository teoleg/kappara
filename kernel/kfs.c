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
		if (kf->bd->bd_read(kf->bd, 1, dir) == 0) {
			dir[kf->dirent_idx].size_bytes = kf->size_bytes;
			kf->bd->bd_write(kf->bd, 1, dir);
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

static struct file_ops regfile_fops = {
	.open  = regfile_open,
	.close = regfile_close,
	.read  = regfile_read,
	.write = regfile_write,
	.seek  = regfile_seek,
};

/*
 * Per-mount creat state.  Tracks the block device backing the kfs
 * mount + the next free block in the linear allocation pool.  We
 * only support one kfs mount today so this is a single static;
 * generalising to multiple mounts means turning kfs_mnt into a
 * per-mount object reachable via the directory inode's i_private.
 */
static struct {
	struct block_device *bd;
	uint32_t             next_free_block;
} kfs_mnt;

static struct file_ops kfs_dir_fops;	/* fwd; defined below */

/* Allocate one block from the linear pool and zero it.  Returns the
 * allocated block number; updates the superblock so next_free_block
 * survives a remount.  Returns 0 on failure (block 0 is the super,
 * never returned as data). */
static uint32_t kfs_alloc_block_zero(struct block_device *bd, uint32_t count)
{
	uint32_t start = kfs_mnt.next_free_block;
	unsigned char zero[BLK_SIZE];
	for (size_t i = 0; i < BLK_SIZE; i++) zero[i] = 0;
	for (uint32_t b = 0; b < count; b++)
		if (bd->bd_write(bd, start + b, zero) < 0) return 0;
	kfs_mnt.next_free_block += count;
	struct kfs_super sb;
	if (bd->bd_read(bd, 0, &sb) == 0) {
		sb.next_free_block = kfs_mnt.next_free_block;
		bd->bd_write(bd, 0, &sb);
	}
	return start;
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

static struct file_ops kfs_dir_fops = {
	.creat = kfs_dir_creat,
	.mkdir = kfs_dir_mkdir,
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
	sb.magic           = KFS_MAGIC;
	sb.num_files       = NPAYLOADS;
	/* next_free_block filled in after we know how many blocks
	 * the canned files take up; see end of function. */
	bd->bd_write(bd, 0, &sb);

	/* Directory at block 1; up to KFS_DIRENTS entries. */
	struct kfs_dirent dir[KFS_DIRENTS];
	kmemset(dir, 0, sizeof(dir));

	/*
	 * Each file gets a fixed KFS_BLOCKS_PER_FILE-block slot so it
	 * can be overwritten or extended in place up to that size, with
	 * no free-block management needed.  Unused trailing blocks are
	 * zeroed so they read as zeros if the file later grows into them.
	 */
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

		dir[i].type        = KFS_TYPE_FILE;
		const char *src = p->data;
		uint32_t    rem = p->size;
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
		}
		cur_block += KFS_BLOCKS_PER_FILE;
	}
	bd->bd_write(bd, 1, dir);

	/* Re-write the superblock with the final next_free_block. */
	sb.next_free_block = cur_block;
	bd->bd_write(bd, 0, &sb);

	kprintf("kfs: mkimage wrote %u files (blocks used: 0..%u, "
		"next_free_block=%u)\n",
		(unsigned)NPAYLOADS, (unsigned)(cur_block - 1),
		(unsigned)cur_block);
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
	if (bd->bd_read(bd, 0, &sb) < 0) {
		kprintf("kfs_mount: superblock read failed\n");
		return -1;
	}
	if (sb.magic != KFS_MAGIC) {
		kprintf("kfs_mount: bad magic 0x%lx (want 0x%lx)\n",
			(unsigned long)sb.magic, (unsigned long)KFS_MAGIC);
		return -1;
	}

	kfs_mnt.bd              = bd;
	kfs_mnt.next_free_block = sb.next_free_block;

	/* Publish creat/mkdir on the mountpoint's inode and stash a
	 * kfs_dir so kfs_dir_creat / kfs_dir_mkdir know which dir
	 * block to operate on. */
	struct kfs_dir *root = kmalloc(sizeof(*root));
	if (!root) return -1;
	root->bd        = bd;
	root->dir_block = 1;
	root->dentry    = mountpoint;
	mountpoint->d_inode->i_fops    = &kfs_dir_fops;
	mountpoint->d_inode->i_private = root;

	kfs_mount_dir(bd, 1, mountpoint);

	kprintf("kfs: mounted '%s' on /%s\n",
		bd->bd_name, mountpoint->d_name);
	return 0;
}

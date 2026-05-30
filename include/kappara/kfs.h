/*
 * include/kappara/kfs.h -- "kappara file system" v1
 *
 * The smallest interesting on-disk format: a flat directory of small
 * files.  No hierarchy yet (no subdirs within kfs), no free-block
 * management (files allocated at mkimage time get fixed homes).
 *
 * Disk layout (each block = 512 B):
 *
 *   block 0             KFS superblock
 *                         magic  ('KFS\1' = 0x014b4653 LE)
 *                         num_files
 *
 *   block 1             root directory
 *                         16 dirent slots, each:
 *                            name[24]      NUL-terminated short name
 *                            start_block   first block of file's data
 *                            size_bytes    file length
 *                         All fields little-endian.
 *
 *   block 2..N          file data (contiguous, never fragmented in v1)
 *
 * Mount semantics
 * ---------------
 *   kfs_mkimage(bd)     write a fresh, hand-rolled image into bd
 *                       (used at boot since we have no formatter)
 *   kfs_mount(bd, dir)  read the dir; for each dirent, vfs_mknod_reg
 *                       a regfile node under `dir` whose i_private
 *                       points at a struct kfs_file (bd + start + size)
 *
 * After mount, sys_read on the file goes through regfile_fops which
 * pulls blocks via bd->bd_read on demand.
 */

#ifndef KAPPARA_KFS_H
#define KAPPARA_KFS_H

#include <stdint.h>

#include "kappara/blkdev.h"

#define KFS_MAGIC	0x014b4653u	/* 'KFS\1' little-endian */

struct kfs_super {
	uint32_t	magic;
	uint32_t	num_files;
	uint8_t		pad[BLK_SIZE - 8];
};

#define KFS_NAME_MAX	24
#define KFS_DIRENTS	16		/* per directory block */

struct kfs_dirent {
	char		name[KFS_NAME_MAX];
	uint32_t	start_block;
	uint32_t	size_bytes;
};

/* In-memory per-file metadata pointed at by inode i_private. */
struct kfs_file {
	struct block_device	*bd;
	uint32_t		 start_block;
	uint32_t		 size_bytes;
};

/* Forward decl for vfs.h dependency. */
struct dentry;

void kfs_mkimage(struct block_device *bd);
int  kfs_mount  (struct block_device *bd, struct dentry *mountpoint);

#endif

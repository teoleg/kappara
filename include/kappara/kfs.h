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
	uint32_t	next_free_block;	/* first block past all files */
	uint8_t		pad[BLK_SIZE - 12];
};

#define KFS_NAME_MAX	24
#define KFS_DIRENTS	14		/* per directory block (36-byte slots) */

#define KFS_TYPE_FILE	0
#define KFS_TYPE_DIR	1

struct kfs_dirent {
	char		name[KFS_NAME_MAX];
	uint32_t	start_block;	/* for files: first data block      */
					/* for dirs:  block of the subdir's */
					/*            own dirent table       */
	uint32_t	size_bytes;	/* file size in bytes; 0 for dirs   */
	uint32_t	type;		/* KFS_TYPE_FILE / _DIR             */
};

/* Each file is given a fixed number of contiguous blocks at mkimage
 * time so it has room to grow when written.  Real on-disk filesystems
 * use a free-block bitmap + indirect blocks; we cheat. */
#define KFS_BLOCKS_PER_FILE	4

/* In-memory per-file metadata pointed at by inode i_private. */
struct kfs_file {
	struct block_device	*bd;
	uint32_t		 start_block;
	uint32_t		 size_bytes;
	uint32_t		 alloc_blocks;	/* fixed at mkimage */
	uint32_t		 dir_block;	/* block of the parent dir's   */
						/* dirent table (so we can     */
						/* push size updates back)     */
	uint8_t			 dirent_idx;	/* slot in that dir block */
};

/* In-memory per-directory metadata.  Directory inodes' i_private
 * points to one of these.  The mountpoint's dir_block is 1 (the
 * root directory block written by kfs_mkimage); subdirs get their
 * own block when created by kfs_dir_mkdir. */
struct kfs_dir {
	struct block_device	*bd;
	uint32_t		 dir_block;
	struct dentry		*dentry;
};

/* Forward decl for vfs.h dependency. */
struct dentry;

void kfs_mkimage(struct block_device *bd);
int  kfs_mount  (struct block_device *bd, struct dentry *mountpoint);

#endif

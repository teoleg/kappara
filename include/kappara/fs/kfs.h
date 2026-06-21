/*
 * include/kappara/fs/kfs.h -- "kappara file system" v2
 *
 * Disk layout (each block = 512 B):
 *
 *   block 0   KFS superblock      magic, num_files
 *   block 1   free-block bitmap   one bit per block, LSB-first within
 *                                  each byte.  1 = allocated, 0 = free.
 *                                  Fits 4096 blocks per 512 B; we use
 *                                  only the first N bits for an N-block
 *                                  device.
 *   block 2   root dirent table   up to KFS_DIRENTS entries
 *   block 3.. data + subdir dirent tables
 *
 *   each dirent (36 bytes):
 *     char     name[24];
 *     uint32_t start_block;   // file: first data block
 *                             // dir:  the subdir's own dirent table
 *     uint32_t size_bytes;
 *     uint32_t type;          // KFS_TYPE_FILE / KFS_TYPE_DIR
 *
 * v2 vs v1
 * --------
 * v1 had no bitmap.  Block allocation was a monotonic counter stored
 * in the superblock, so deleted files leaked their blocks forever.
 * v2 replaces that counter with a real bitmap.  kfs_alloc_block scans
 * for the first N contiguous free blocks; kfs_free_block returns
 * them to the pool.  rm and rmdir now actually reclaim space.
 */

#ifndef KAPPARA_KFS_H
#define KAPPARA_KFS_H

#include <stdint.h>

#include "kappara/fs/blkdev.h"

#define KFS_MAGIC	0x014b4653u	/* 'KFS\1' little-endian */

#define KFS_SUPER_BLOCK		0
#define KFS_BITMAP_BLOCK	1
#define KFS_ROOT_BLOCK		2

struct kfs_super {
	uint32_t	magic;
	uint32_t	num_files;
	uint8_t		pad[BLK_SIZE - 8];
};

/* Filesystem-wide name limit and per-dir-block entry cap.  Each
 * kfs_dirent is `KFS_NAME_MAX + 12` bytes (name + start_block + size
 * + type).  We pack as many as fit in one 512 B block, so the two
 * constants move together: 11 / 22 give 11*22 + 12*22 = 506 B used.
 * History: 24/14 → 16/18 → 12/21 → 11/22 as /usr/bin grew.  Longest
 * name shipped is "tcpconnect" (10 chars + NUL = 11). */
#define KFS_NAME_MAX	11
#define KFS_DIRENTS	22

#define KFS_TYPE_FILE	0
#define KFS_TYPE_DIR	1

/* Packed because KFS_NAME_MAX no longer needs to be 4-byte aligned --
 * the struct's on-disk layout is fixed at exactly KFS_NAME_MAX + 12
 * bytes so that the DIRENTS-per-block math matches the comments above. */
struct kfs_dirent {
	char		name[KFS_NAME_MAX];
	uint32_t	start_block;	/* for files: first data block      */
					/* for dirs:  block of the subdir's */
					/*            own dirent table       */
	uint32_t	size_bytes;	/* file size in bytes; 0 for dirs   */
	uint32_t	type;		/* KFS_TYPE_FILE / _DIR             */
} __attribute__((packed));

/* Each file is given a fixed number of contiguous blocks at mkimage
 * time so it has room to grow when written.  Real on-disk filesystems
 * use a free-block bitmap + indirect blocks; we cheat.  32 blocks =
 * 32 KB per file -- bumped from 16 KB when cmd/test consolidated
 * all selftests into one ~20 KB binary. */
#define KFS_BLOCKS_PER_FILE	64

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

/* mkimage payload entry: one file's name and bytes.  kfs_mkimage
 * lays these out in order at root level, each in its own
 * KFS_BLOCKS_PER_FILE-block slot.  Pass NULL/0 for an empty
 * filesystem. */
struct kfs_payload {
	const char *name;
	const void *data;
	uint32_t    size;
};

void kfs_mkimage(struct block_device *bd,
		 const struct kfs_payload *payloads, unsigned n);
int  kfs_mount  (struct block_device *bd, struct dentry *mountpoint);

#endif

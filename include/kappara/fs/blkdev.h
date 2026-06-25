/*
 * include/kappara/blkdev.h -- block device handle (SVR4-style)
 *
 * `struct block_device` is the high-level handle filesystems carry
 * around: a name + size + dev_t.  Actual I/O goes through the
 * buffer cache and the bdevsw[] switch -- this struct does NOT
 * hold function pointers anymore.  It's the analog of a vnode for
 * a disk: identity + size, not behaviour.
 *
 *   user/syscall
 *       |
 *       v
 *   VFS  +-- INODE_REG -> regfile_fops -> kfs_read --+
 *        \-- INODE_DIR                               |
 *                                                    v
 *                                          bread / bwrite (fs/buf.h)
 *                                                    |
 *                                                    v
 *                                          bdevsw[major].d_strategy(buf*)
 *                                                    |
 *                                                    v
 *                                            ramdisk / nvme / ...
 *
 * Filesystem code never touches the driver directly; it asks the
 * buffer cache for a buf and either reads it (B_VALID set after
 * the strategy returns) or dirties it and bwrites.
 *
 * Sector size is fixed at 512 bytes -- the de facto block-device
 * granularity since the floppy era.  Real SDHCI/NVMe/USB are happy
 * to do 512 too, so we don't lose anything.
 */

#ifndef KAPPARA_BLKDEV_H
#define KAPPARA_BLKDEV_H

#include <stddef.h>
#include <stdint.h>

#include "kappara/io/cdevsw.h"	/* dev_t / MAJOR / MINOR / MKDEV */

#define BLK_SIZE	512

struct block_device {
	const char *bd_name;	/* "ramdisk0", "nvme0n1", ... */
	uint32_t    bd_nblocks;	/* device capacity in BLK_SIZE units */
	dev_t       bd_dev;	/* MKDEV(bdevsw major, minor) */
};

void                  ramdisk_init(void);
struct block_device  *ramdisk_get(void);

/* Second ramdisk for the writable /home mount (separate from
 * /usr/bin so an FTP STOR can't overwrite an executable in flight).
 * Same shape as ramdisk0, distinct storage + name. */
void                  ramdisk_home_init(void);
struct block_device  *ramdisk_home_get(void);

#endif

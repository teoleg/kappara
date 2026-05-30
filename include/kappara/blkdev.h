/*
 * include/kappara/blkdev.h -- block device abstraction
 *
 * The thinnest plausible interface for "something I can read 512-byte
 * blocks out of and write 512-byte blocks into".  Filesystem drivers
 * (kfs today; future ext2/fat32) talk to storage through this; real
 * drivers (today: ramdisk; future: virtio-blk, SDHCI) implement it.
 *
 *   user/syscall
 *       |
 *       v
 *   VFS  +-- INODE_REG -> regfile_fops -> kfs_read --+
 *        \-- INODE_DIR                               |
 *                                                    v
 *                                          struct block_device
 *                                          (read_block / write_block)
 *                                                    |
 *                                                    v
 *                                            ramdisk / virtio-blk / ...
 *
 * Sector size is fixed at 512 bytes -- the de facto block-device
 * granularity since the floppy era.  Real SDHCI/NVMe/USB are happy
 * to do 512 too, so we don't lose anything.
 */

#ifndef KAPPARA_BLKDEV_H
#define KAPPARA_BLKDEV_H

#include <stddef.h>
#include <stdint.h>

#define BLK_SIZE	512

struct block_device {
	const char	*bd_name;
	uint32_t	 bd_nblocks;
	int		(*bd_read )(struct block_device *bd,
				    uint32_t blkno, void *buf);
	int		(*bd_write)(struct block_device *bd,
				    uint32_t blkno, const void *buf);
};

void                  ramdisk_init(void);
struct block_device  *ramdisk_get(void);

#endif

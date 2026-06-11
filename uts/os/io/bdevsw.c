/*
 * kernel/io/bdevsw.c -- block-device switch
 *
 * Trivial parallel to cdevsw.c: 16-entry table, register + lookup.
 * Real I/O dispatch happens in the buffer cache (buf.c) which calls
 * into bdev_entry::read_block / ::write_block as cache misses
 * happen.
 *
 * Phase S1.
 */

#include "kappara/fs/bdevsw.h"

static struct bdev_entry *bdev_table[BDEV_MAX];

int bdev_register(unsigned major, struct bdev_entry *e)
{
	if (major == 0 || major >= BDEV_MAX) return -1;
	if (!e) return -1;
	if (bdev_table[major]) return -1;
	bdev_table[major] = e;
	return 0;
}

struct bdev_entry *bdev_lookup(unsigned major)
{
	if (major == 0 || major >= BDEV_MAX) return 0;
	return bdev_table[major];
}

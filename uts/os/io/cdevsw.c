/*
 * kernel/cdevsw.c -- SVR4 character-device switch
 *
 * Tiny fixed-size table indexed by major.  Drivers register at boot
 * via cdev_register; the VFS open path calls cdev_lookup to find
 * the streamtab and constructs a stream around it.
 *
 * No dynamic majors yet -- everyone takes their well-known number
 * from cdevsw.h.  When the table fills up (CDEV_MAX = 32) we grow.
 */

#include <stddef.h>

#include "kappara/io/cdevsw.h"
#include "kappara/core/printk.h"

static struct cdev_entry cdevsw[CDEV_MAX];

int cdev_register(unsigned major, const char *name, struct streamtab *st)
{
	if (major >= CDEV_MAX) {
		kprintf("cdev_register: major %u out of range (max %u)\n",
			major, (unsigned)CDEV_MAX);
		return -1;
	}
	if (cdevsw[major].streamtab) {
		kprintf("cdev_register: major %u already taken by '%s'\n",
			major, cdevsw[major].name);
		return -1;
	}
	cdevsw[major].name      = name;
	cdevsw[major].streamtab = st;
	return 0;
}

struct cdev_entry *cdev_lookup(unsigned major)
{
	if (major >= CDEV_MAX) return NULL;
	if (!cdevsw[major].streamtab) return NULL;
	return &cdevsw[major];
}

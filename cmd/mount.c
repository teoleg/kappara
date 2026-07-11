/*
 * cmd/mount.c -- list mounted filesystems
 *
 * Just cats /proc/mounts.  Real Unix `mount(8)` with no args does
 * effectively the same thing; the kernel keeps the live mount
 * table and userland is the formatter.
 *
 * We don't take mount/unmount args yet -- the kernel's
 * kfs_mount() is currently invoked exclusively from
 * exec_space_init at boot, so there's nothing to drive from EL0.
 * When sys_mount lands, this command grows the device + path
 * args.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int strip = (argc > 1 && argv[1][0] == '-'
	             && argv[1][1] == '\0');
	if (proc_cat("/proc/mounts", strip) < 0) {
		write(2, "mount: cannot open /proc/mounts\n", 32);
		return 1;
	}
	return 0;
}

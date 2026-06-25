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
#include <unistd.h>

int main(int argc, char **argv)
{
	(void)argv;
	if (argc > 1) {
		write(2, "usage: mount\n", 13);
		write(2, "(runtime mount/unmount not implemented yet)\n", 44);
		return 1;
	}
	int fd = open("/proc/mounts", 0);
	if (fd < 0) {
		write(2, "mount: cannot open /proc/mounts\n", 32);
		return 1;
	}
	char buf[256];
	for (;;) {
		long n = read(fd, buf, sizeof(buf));
		if (n <= 0) break;
		write(1, buf, (size_t)n);
	}
	close(fd);
	return 0;
}

/*
 * cmd/ifconfig.c -- print network interface state.
 *
 * Reads /proc/netif (a STREAMS snapshot cdev) and dumps it to stdout.
 * Read-only; we don't have a way to mutate netifs yet -- they're all
 * statically registered at boot.  Once SLIP/Ethernet drivers land with
 * runtime-configurable state, this becomes the natural place to drive
 * an ioctl for "ifconfig lo0 up", "ifconfig slip0 192.168.1.2", etc.
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	int fd = open("/proc/netif", 0);
	if (fd < 0) {
		puts("ifconfig: cannot open /proc/netif");
		return 1;
	}
	char buf[1024];
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		write(1, buf, (size_t)n);
	close(fd);
	return 0;
}

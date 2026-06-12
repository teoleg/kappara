/*
 * cmd/netstats -- network state inspector.
 *
 * Concatenates the per-subsystem /proc snapshots (netif, slip, tcp)
 * into one human-readable dump.  Same role as netstat / ss on real
 * Unix, but reading directly from procfs cdevs rather than a
 * dedicated kernel ABI.
 */

#include <stdio.h>
#include <unistd.h>

static void cat(const char *path)
{
	int fd = open(path, 0);
	if (fd < 0) {
		printf("netstats: cannot open %s\n", path);
		return;
	}
	char buf[1024];
	int n;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		write(1, buf, (unsigned)n);
	close(fd);
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	puts("-- interfaces (/proc/netif) --");
	cat("/proc/netif");

	puts("-- slip0 counters (/proc/slip) --");
	cat("/proc/slip");

	puts("-- tcp connections (/proc/tcp) --");
	cat("/proc/tcp");

	return 0;
}

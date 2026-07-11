/*
 * cmd/netstat -- network state inspector.
 *
 * Concatenates the per-subsystem /proc snapshots (netif, slip, tcp)
 * into one human-readable dump.  Same role as netstat / ss on real
 * Unix, but reading directly from procfs cdevs rather than a
 * dedicated kernel ABI.
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{

	int strip = (argc > 1 && argv[1][0] == '-' && argv[1][1] == '\0');
	puts("-- interfaces (/proc/netif) --");
	proc_cat("/proc/netif", strip);

	puts("-- slip0 counters (/proc/slip) --");
	proc_cat("/proc/slip", strip);

	puts("-- tcp connections (/proc/tcp) --");
	proc_cat("/proc/tcp", strip);

	return 0;
}

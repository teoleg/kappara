/*
 * cmd/ifconfig.c -- show / plumb network interfaces.
 *
 * Show:  ifconfig                     (dumps /proc/netif)
 * Get:   ifconfig eth0                (one interface via SIOCGIF*)
 * Set:   ifconfig eth0 <ip> [netmask <mask>] [gw <gateway>]
 *
 * Plumbing rides SIOCSIF* / SIOCGIF* ioctls (kappara/net/sockio.h)
 * down /dev/udp -- the ip multiplexor at the bottom of that stream
 * answers them.  Same shape as Solaris ifconfig over /dev/udp.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stropts.h>
#include <unistd.h>
#include "kappara/net/sockio.h"

static int parse_ipv4(const char *s, uint32_t *out)
{
	uint32_t ip = 0;
	for (int i = 3; i >= 0; i--) {
		if (*s < '0' || *s > '9') return -1;
		unsigned octet = 0;
		while (*s >= '0' && *s <= '9')
			octet = octet * 10 + (unsigned)(*s++ - '0');
		if (octet > 255) return -1;
		ip |= (uint32_t)octet << (i * 8);
		if (i > 0) {
			if (*s != '.') return -1;
			s++;
		}
	}
	if (*s) return -1;
	*out = ip;
	return 0;
}

static void print_ip(uint32_t ip)
{
	printf("%u.%u.%u.%u",
	       (unsigned)(ip >> 24) & 0xff, (unsigned)(ip >> 16) & 0xff,
	       (unsigned)(ip >>  8) & 0xff, (unsigned) ip        & 0xff);
}

static void ifr_init(struct kifreq *ifr, const char *name)
{
	memset(ifr, 0, sizeof(*ifr));
	unsigned i = 0;
	while (name[i] && i < sizeof(ifr->ifr_name) - 1) {
		ifr->ifr_name[i] = name[i];
		i++;
	}
}

static int show_all(void)
{
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

static int show_one(int fd, const char *name)
{
	struct kifreq ifr;

	ifr_init(&ifr, name);
	if (ioctl(fd, SIOCGIFADDR, (long)&ifr) < 0) {
		printf("ifconfig: no such interface '%s'\n", name);
		return 1;
	}
	printf("%s: ", name);
	print_ip(ifr.ifr_addr);

	ifr_init(&ifr, name);
	if (ioctl(fd, SIOCGIFNETMASK, (long)&ifr) == 0) {
		printf(" netmask ");
		print_ip(ifr.ifr_addr);
	}
	ifr_init(&ifr, name);
	if (ioctl(fd, SIOCGIFGW, (long)&ifr) == 0 && ifr.ifr_addr) {
		printf(" gw ");
		print_ip(ifr.ifr_addr);
	}
	ifr_init(&ifr, name);
	if (ioctl(fd, SIOCGIFHWADDR, (long)&ifr) == 0)
		printf(" ether %02x:%02x:%02x:%02x:%02x:%02x",
		       ifr.ifr_mac[0], ifr.ifr_mac[1], ifr.ifr_mac[2],
		       ifr.ifr_mac[3], ifr.ifr_mac[4], ifr.ifr_mac[5]);
	printf("\n");
	return 0;
}

static int set_one(int fd, const char *name, int cmd, const char *val)
{
	struct kifreq ifr;
	uint32_t ip;

	if (parse_ipv4(val, &ip) < 0) {
		printf("ifconfig: bad address '%s'\n", val);
		return 1;
	}
	ifr_init(&ifr, name);
	ifr.ifr_addr = ip;
	if (ioctl(fd, cmd, (long)&ifr) < 0) {
		printf("ifconfig: ioctl 0x%x failed for '%s'\n",
		       (unsigned)cmd, name);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return show_all();

	int fd = open("/dev/udp", 0);
	if (fd < 0) {
		puts("ifconfig: cannot open /dev/udp");
		return 1;
	}

	const char *name = argv[1];
	int rc = 0;

	if (argc == 2) {
		rc = show_one(fd, name);
	} else {
		/* ifconfig <if> <ip> [netmask <m>] [gw <g>] */
		rc = set_one(fd, name, SIOCSIFADDR, argv[2]);
		for (int i = 3; rc == 0 && i + 1 < argc; i += 2) {
			if (strcmp(argv[i], "netmask") == 0)
				rc = set_one(fd, name, SIOCSIFNETMASK,
					     argv[i + 1]);
			else if (strcmp(argv[i], "gw") == 0)
				rc = set_one(fd, name, SIOCSIFGW,
					     argv[i + 1]);
			else {
				printf("ifconfig: unknown option '%s'\n",
				       argv[i]);
				rc = 1;
			}
		}
		if (rc == 0)
			rc = show_one(fd, name);
	}
	close(fd);
	return rc;
}

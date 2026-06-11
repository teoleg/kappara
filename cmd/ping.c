/*
 * cmd/ping.c -- send ICMP echo requests via /dev/icmp
 *
 * Usage:  ping [ip-address] [count]
 *   ip-address  dotted-quad IPv4 (default: 127.0.0.1)
 *   count       number of pings (default: 4)
 *
 * Protocol: write struct icmp_ping_req to /dev/icmp,
 *           read  struct icmp_ping_rep back.
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include "kappara/net/icmp.h"

/* Parse "a.b.c.d" into host-byte-order uint32_t.
 * Returns 0 on success, -1 on bad input. */
static int parse_ipv4(const char *s, uint32_t *out)
{
	uint32_t ip = 0;
	for (int i = 3; i >= 0; i--) {
		if (*s < '0' || *s > '9')
			return -1;
		unsigned octet = 0;
		while (*s >= '0' && *s <= '9')
			octet = octet * 10 + (unsigned)(*s++ - '0');
		if (octet > 255)
			return -1;
		ip |= ((uint32_t)octet << (i * 8));
		if (i > 0) {
			if (*s != '.')
				return -1;
			s++;
		}
	}
	*out = ip;
	return 0;
}

static int do_ping(int fd, uint32_t dst_ip, uint16_t seq)
{
	struct icmp_ping_req req;
	req.dst_ip = dst_ip;
	req.seq    = seq;
	req._pad   = 0;

	if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
		puts("ping: write failed");
		return -1;
	}

	struct icmp_ping_rep rep;
	ssize_t n = read(fd, &rep, sizeof(rep));
	if (n < (ssize_t)sizeof(rep)) {
		puts("ping: no reply (timeout)");
		return -1;
	}

	uint32_t s = rep.src_ip;
	printf("PONG from %u.%u.%u.%u: seq=%u rtt=%dms\n",
	       (s >> 24) & 0xff, (s >> 16) & 0xff,
	       (s >>  8) & 0xff,  s        & 0xff,
	       (unsigned)rep.seq, (int)rep.rtt_ms);
	return 0;
}

int main(int argc, char **argv)
{
	uint32_t dst = 0x7f000001u;	/* 127.0.0.1 */
	int count = 4;

	if (argc >= 2) {
		if (parse_ipv4(argv[1], &dst) < 0) {
			puts("ping: invalid address");
			return 1;
		}
	}
	if (argc >= 3) {
		int c = 0;
		const char *p = argv[2];
		while (*p >= '0' && *p <= '9')
			c = c * 10 + (*p++ - '0');
		if (c > 0 && c <= 64)
			count = c;
	}

	int fd = open("/dev/icmp", 2);	/* O_RDWR */
	if (fd < 0) {
		puts("ping: cannot open /dev/icmp");
		return 1;
	}

	uint32_t s = dst;
	printf("PING %u.%u.%u.%u via lo0\n",
	       (s >> 24) & 0xff, (s >> 16) & 0xff,
	       (s >>  8) & 0xff,  s        & 0xff);

	int ok = 0, fail = 0;
	for (int i = 0; i < count; i++) {
		if (do_ping(fd, dst, (uint16_t)(i + 1)) == 0)
			ok++;
		else
			fail++;
	}

	printf("%d transmitted, %d received\n", count, ok);
	close(fd);
	return (fail > 0) ? 1 : 0;
}

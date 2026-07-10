/*
 * cmd/host.c -- DNS A-record lookup.  Usage:  host <name>
 *
 * Talks UDP to the SLIRP DNS forwarder at 10.0.2.3:53 (QEMU's
 * default user-mode network).  Builds an RFC 1035 standard query
 * for the A record, parses the answer section, prints any IPv4
 * addresses it finds.
 *
 * Server selection: SIOCGIFDNS on eth0 (plumbed by dhcpagent from
 * DHCP option 6 -- kappara's resolv.conf stand-in), falling back
 * to SLIRP's 10.0.2.3 when nothing is plumbed.
 *
 * Reliability: I_SRDTMO arms a 3 s reply timeout and the query is
 * retried twice -- UDP to a fresh destination loses the first
 * packet to the ARP exchange on a cold cache, and DNS over UDP is
 * lossy by design anyway.
 *
 * Limitations on purpose:
 *   - A-records only.  No AAAA / CNAME chasing / SRV / MX.
 *   - Names up to 255 bytes (DNS protocol max).
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stropts.h>

#include "kappara/net/sockio.h"
#include "kappara/net/udp.h"

#define DNS_FALLBACK_IP 0x0A000203u	/* 10.0.2.3 (SLIRP forwarder)   */
#define DNS_SERVER_PORT 53

/* DNS server plumbed by dhcpagent (option 6); fallback for rigs
 * with no lease. */
static uint32_t dns_server(int udp_fd)
{
	struct kifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "eth0");
	if (ioctl(udp_fd, SIOCGIFDNS, (long)&ifr) == 0 && ifr.ifr_addr)
		return ifr.ifr_addr;
	return DNS_FALLBACK_IP;
}

/* RFC 1035 §4.1.1: 12-byte header. */
struct dns_hdr {
	uint16_t id;
	uint16_t flags;
	uint16_t qdcount;
	uint16_t ancount;
	uint16_t nscount;
	uint16_t arcount;
};

static uint16_t be16(uint16_t x)
{
	return (uint16_t)((x << 8) | (x >> 8));
}

static void put_u16(uint8_t **p, uint16_t v)
{
	(*p)[0] = (uint8_t)(v >> 8);
	(*p)[1] = (uint8_t) v;
	*p += 2;
}

/* Encode "google.com" -> "\x06google\x03com\x00" in place.  Returns
 * the number of bytes written, or -1 if the name is malformed
 * (empty label, label > 63 bytes, total > 255). */
static int encode_qname(const char *name, uint8_t *out, int cap)
{
	int n = 0;
	const char *p = name;
	while (*p) {
		const char *seg = p;
		while (*p && *p != '.') p++;
		int seglen = (int)(p - seg);
		if (seglen == 0 || seglen > 63) return -1;
		if (n + 1 + seglen + 1 > cap)   return -1;
		out[n++] = (uint8_t)seglen;
		for (int i = 0; i < seglen; i++) out[n++] = (uint8_t)seg[i];
		if (*p == '.') p++;
	}
	out[n++] = 0;	/* root label */
	return n;
}

/* Skip a DNS name in `msg` starting at `off`.  Handles 0xC0 pointer
 * compression by stopping after the pointer byte pair (we don't
 * follow it; callers don't need the decoded name).  Returns the
 * offset just past the name, or -1 on overrun. */
static int skip_name(const uint8_t *msg, int len, int off)
{
	while (off < len) {
		uint8_t b = msg[off];
		if (b == 0) return off + 1;
		if ((b & 0xC0) == 0xC0) {
			if (off + 1 >= len) return -1;
			return off + 2;
		}
		if (b > 63) return -1;
		off += 1 + b;
	}
	return -1;
}

static void print_ip(uint32_t ip_be)
{
	char line[24];
	int  n = 0;
	for (int shift = 24; shift >= 0; shift -= 8) {
		uint32_t b = (ip_be >> shift) & 0xff;
		if (b >= 100) {
			line[n++] = (char)('0' + b / 100);
			line[n++] = (char)('0' + (b / 10) % 10);
			line[n++] = (char)('0' + b % 10);
		} else if (b >= 10) {
			line[n++] = (char)('0' + b / 10);
			line[n++] = (char)('0' + b % 10);
		} else {
			line[n++] = (char)('0' + b);
		}
		if (shift) line[n++] = '.';
	}
	line[n++] = '\n';
	write(1, line, (size_t)n);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		write(2, "usage: host <name>\n", 19);
		return 1;
	}
	const char *name = argv[1];

	int fd = open("/dev/udp", 2);
	if (fd < 0) { write(2, "host: open /dev/udp failed\n", 27); return 1; }

	/* Bind ephemeral (port=0 -> kernel picks one). */
	struct t_bind_req breq = { .prim = T_BIND_REQ, .port = 0 };
	struct strbuf bctl = { .maxlen = 0,
	                       .len = sizeof(breq), .buf = &breq };
	if (putmsg(fd, &bctl, NULL, 0) < 0) {
		write(2, "host: bind failed\n", 18); close(fd); return 1;
	}
	struct t_bind_ack back;
	struct strbuf bcin = { .maxlen = sizeof(back),
	                       .len = 0, .buf = &back };
	int bflags = 0;
	if (getmsg(fd, &bcin, NULL, &bflags) < 0 || back.prim != T_BIND_ACK) {
		write(2, "host: bind ack failed\n", 22);
		close(fd); return 1;
	}

	/* Build the DNS query: header + one Question. */
	uint8_t pkt[300];
	uint8_t *p = pkt;
	put_u16(&p, 0xBEEF);		/* ID (any) */
	put_u16(&p, 0x0100);		/* flags: standard query, RD=1 */
	put_u16(&p, 1);			/* qdcount */
	put_u16(&p, 0); put_u16(&p, 0); put_u16(&p, 0);
	int qn = encode_qname(name, p, (int)(sizeof(pkt) - (p - pkt) - 4));
	if (qn < 0) {
		write(2, "host: bad name\n", 15); close(fd); return 1;
	}
	p += qn;
	put_u16(&p, 1);			/* QTYPE = A */
	put_u16(&p, 1);			/* QCLASS = IN */
	int plen = (int)(p - pkt);

	/* 3 s reply timeout; 3 attempts.  First packet to a fresh
	 * destination can be lost to the cold-cache ARP exchange. */
	ioctl(fd, I_SRDTMO, 3000);

	struct t_unitdata_req ureq = {
		.prim = T_UNITDATA_REQ,
		.dst_ip = dns_server(fd), .dst_port = DNS_SERVER_PORT,
	};
	struct t_unitdata_ind ind;
	uint8_t rbuf[600];
	struct strbuf rctl = { .maxlen = sizeof(ind),
	                       .len = 0, .buf = &ind };
	struct strbuf rdat = { .maxlen = sizeof(rbuf),
	                       .len = 0, .buf = rbuf };
	int got = -1;
	for (int attempt = 0; attempt < 3 && got < 0; attempt++) {
		struct strbuf uctl = { .maxlen = 0,
		                       .len = sizeof(ureq), .buf = &ureq };
		struct strbuf udat = { .maxlen = 0,
		                       .len = plen, .buf = pkt };
		if (putmsg(fd, &uctl, &udat, 0) < 0) {
			write(2, "host: send failed\n", 18);
			close(fd); return 1;
		}
		int rflags = 0;
		rctl.len = rdat.len = 0;
		got = (int)getmsg(fd, &rctl, &rdat, &rflags);
	}
	if (got < 0) {
		write(2, "host: no reply (timeout)\n", 25);
		close(fd); return 1;
	}
	close(fd);

	/* Parse the answer section.  Header is 12 bytes; skip the
	 * Question (NAME + 4); then walk ANCOUNT records. */
	if (rdat.len < (int)sizeof(struct dns_hdr)) {
		write(2, "host: short reply\n", 18); return 1;
	}
	const struct dns_hdr *h = (const struct dns_hdr *)rbuf;
	int ancount = be16(h->ancount);
	int rcode   = be16(h->flags) & 0x000F;
	if (rcode == 3)  { write(2, "host: NXDOMAIN\n", 15); return 1; }
	if (rcode != 0)  { write(2, "host: DNS error\n", 16); return 1; }
	if (ancount == 0){ write(2, "host: no records\n", 17); return 1; }

	int off = (int)sizeof(struct dns_hdr);
	off = skip_name(rbuf, rdat.len, off);
	if (off < 0 || off + 4 > rdat.len) {
		write(2, "host: malformed question\n", 25); return 1;
	}
	off += 4;	/* QTYPE + QCLASS */

	int printed = 0;
	for (int i = 0; i < ancount; i++) {
		off = skip_name(rbuf, rdat.len, off);
		if (off < 0 || off + 10 > rdat.len) break;
		uint16_t type   = (uint16_t)((rbuf[off]     << 8) | rbuf[off + 1]);
		/* rbuf[off+2..3]  = CLASS, rbuf[off+4..7] = TTL */
		uint16_t rdlen  = (uint16_t)((rbuf[off + 8] << 8) | rbuf[off + 9]);
		off += 10;
		if (off + rdlen > rdat.len) break;
		if (type == 1 && rdlen == 4) {
			uint32_t ip = ((uint32_t)rbuf[off]     << 24) |
			              ((uint32_t)rbuf[off + 1] << 16) |
			              ((uint32_t)rbuf[off + 2] <<  8) |
			              ((uint32_t)rbuf[off + 3]);
			print_ip(ip);
			printed++;
		}
		off += rdlen;
	}
	if (!printed) { write(2, "host: no A records\n", 19); return 1; }
	return 0;
}

/*
 * cmd/dhcpagent.c -- userland DHCP client over raw datalink
 *
 * The Solaris shape (docs/DLPI.md): the kernel driver is a dumb
 * datalink; this daemon opens /dev/eth0 raw, runs the RFC 2131
 * DISCOVER/OFFER/REQUEST/ACK exchange building its own Ethernet +
 * IP + UDP frames, then plumbs the lease onto the interface with
 * SIOCSIF* ioctls on /dev/udp and exits.
 *
 * Sequence:
 *   open /dev/eth0
 *   DL_INFO_REQ  -> DL_INFO_ACK           (our MAC + MTU)
 *   DL_BIND_REQ(sap=0x0800) -> DL_BIND_ACK (IPv4 frames only)
 *   ioctl(I_SRDTMO, 2000)                  (2 s read timeout)
 *   3x DISCOVER -> OFFER; 3x REQUEST -> ACK
 *   ioctl(/dev/udp): SIOCSIFADDR/NETMASK/GW
 *
 * On timeout: falls back to QEMU slirp's static topology
 * (10.0.2.15/24 gw 10.0.2.2) so dev boots always get a network.
 * Lease renewal (T1/T2) is future work -- an EC2 VPC address is
 * bound to the ENI for the instance lifetime, and slirp is static.
 *
 * Frame-size notes learned on EC2 (docs/AWS.md), both directions:
 * RX -- real servers send compact replies, so require only the
 * fixed BOOTP header through the magic cookie and bound option
 * parsing by the received length.  TX -- real servers REQUIRE the
 * RFC 2131 300-octet minimum message size and silently drop
 * shorter DISCOVERs; slirp accepts anything, so only AWS catches
 * either mistake.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stropts.h>
#include <unistd.h>
#include "kappara/net/dlpi.h"
#include "kappara/net/sockio.h"

#define ETH_HDR_LEN	14
#define IPV4_HDR_LEN	20
#define UDP_HDR_LEN	8
#define ETHERTYPE_IPV4	0x0800
#define IPPROTO_UDP	17

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC	0x63825363u
#define BOOTREQUEST	1
#define BOOTREPLY	2
#define MSG_DISCOVER	1
#define MSG_OFFER	2
#define MSG_REQUEST	3
#define MSG_ACK		5
#define OPT_PAD		0
#define OPT_SUBNET	1
#define OPT_ROUTER	3
#define OPT_DNS		6
#define OPT_REQ_IP	50
#define OPT_MSG_TYPE	53
#define OPT_SERVER_ID	54
#define OPT_PARAMS	55
#define OPT_END		255

/* Fixed BOOTP header through the magic cookie. */
#define BOOTP_FIXED	240

static uint8_t  our_mac[6];
static uint32_t xid = 0xd1a10001u;

static uint32_t offer_ip, offer_mask, offer_gw, offer_server, offer_dns;

static void be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void be32(uint8_t *p, uint32_t v)
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t rd16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t rd32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
       | ((uint32_t)p[2] << 8) | p[3]; }

static uint16_t csum16(const uint8_t *p, unsigned len, uint32_t seed)
{
	uint32_t s = seed;
	while (len >= 2) { s += ((uint16_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
	if (len) s += (uint16_t)p[0] << 8;
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return (uint16_t)~s;
}

/* Build a DISCOVER or REQUEST frame; returns total length. */
static unsigned build_dhcp(uint8_t *f, uint8_t msg,
			   uint32_t req_ip, uint32_t server)
{
	memset(f, 0, ETH_HDR_LEN + IPV4_HDR_LEN + UDP_HDR_LEN + 300);

	/* BOOTP + options */
	uint8_t *b = f + ETH_HDR_LEN + IPV4_HDR_LEN + UDP_HDR_LEN;
	b[0] = BOOTREQUEST;
	b[1] = 1;		/* htype ethernet */
	b[2] = 6;		/* hlen */
	be32(b + 4, xid);
	be16(b + 10, 0x8000);	/* broadcast flag */
	memcpy(b + 28, our_mac, 6);
	be32(b + 236, DHCP_MAGIC);
	uint8_t *o = b + BOOTP_FIXED;
	*o++ = OPT_MSG_TYPE;  *o++ = 1; *o++ = msg;
	if (msg == MSG_REQUEST) {
		*o++ = OPT_REQ_IP;    *o++ = 4; be32(o, req_ip);  o += 4;
		*o++ = OPT_SERVER_ID; *o++ = 4; be32(o, server);  o += 4;
	}
	*o++ = OPT_PARAMS; *o++ = 3;
	*o++ = OPT_SUBNET; *o++ = OPT_ROUTER; *o++ = OPT_DNS;
	*o++ = OPT_END;
	unsigned blen = (unsigned)(o - b);
	/* RFC 2131: a DHCP message must be at least 300 octets (BOOTP
	 * heritage).  The EC2 VPC responder silently ignores shorter
	 * DISCOVERs; slirp doesn't care, so local tests never catch
	 * this.  The old in-kernel client always sent the full padded
	 * struct, which is why it worked on AWS.  Pad (zeros are
	 * already there from the memset). */
	if (blen < 300)
		blen = 300;

	/* UDP */
	uint8_t *u = f + ETH_HDR_LEN + IPV4_HDR_LEN;
	unsigned ulen = UDP_HDR_LEN + blen;
	be16(u + 0, DHCP_CLIENT_PORT);
	be16(u + 2, DHCP_SERVER_PORT);
	be16(u + 4, (uint16_t)ulen);
	/* pseudo-header: src 0.0.0.0 dst 255.255.255.255 proto 17 */
	uint32_t seed = 0xffff + 0xffff + IPPROTO_UDP + ulen;
	be16(u + 6, csum16(u, ulen, seed));

	/* IP */
	uint8_t *ip = f + ETH_HDR_LEN;
	unsigned tlen = IPV4_HDR_LEN + ulen;
	ip[0] = 0x45;
	be16(ip + 2, (uint16_t)tlen);
	ip[8] = 64;
	ip[9] = IPPROTO_UDP;
	be32(ip + 16, 0xffffffffu);	/* dst 255.255.255.255; src stays 0 */
	be16(ip + 10, csum16(ip, IPV4_HDR_LEN, 0));

	/* Ethernet */
	memset(f, 0xff, 6);
	memcpy(f + 6, our_mac, 6);
	be16(f + 12, ETHERTYPE_IPV4);

	return ETH_HDR_LEN + tlen;
}

/* Parse a frame; if it's a matching OFFER/ACK for our xid, fill the
 * offer_* globals and return the message type, else 0. */
static int parse_reply(const uint8_t *f, unsigned len)
{
	if (len < ETH_HDR_LEN + IPV4_HDR_LEN + UDP_HDR_LEN + BOOTP_FIXED)
		return 0;
	if (rd16(f + 12) != ETHERTYPE_IPV4) return 0;

	const uint8_t *ip = f + ETH_HDR_LEN;
	if ((ip[0] & 0xf0) != 0x40) return 0;
	unsigned ihl = (ip[0] & 0xf) * 4;
	if (ip[9] != IPPROTO_UDP) return 0;

	const uint8_t *u = ip + ihl;
	if (rd16(u + 2) != DHCP_CLIENT_PORT) return 0;

	const uint8_t *b = u + UDP_HDR_LEN;
	if (b[0] != BOOTREPLY) return 0;
	if (rd32(b + 4) != xid) return 0;
	if (rd32(b + 236) != DHCP_MAGIC) return 0;

	uint32_t yiaddr = rd32(b + 16);
	uint32_t mask = 0, gw = 0, server = 0;
	int msg = 0;

	const uint8_t *o    = b + BOOTP_FIXED;
	const uint8_t *oend = f + len;
	while (o < oend && *o != OPT_END) {
		uint8_t code = *o++;
		if (code == OPT_PAD) continue;
		if (o >= oend) break;
		uint8_t olen = *o++;
		if (o + olen > oend) break;
		if      (code == OPT_MSG_TYPE  && olen >= 1) msg    = o[0];
		else if (code == OPT_SUBNET    && olen >= 4) mask   = rd32(o);
		else if (code == OPT_ROUTER    && olen >= 4) gw     = rd32(o);
		else if (code == OPT_DNS       && olen >= 4) offer_dns = rd32(o);
		else if (code == OPT_SERVER_ID && olen >= 4) server = rd32(o);
		o += olen;
	}

	if (msg == MSG_OFFER || msg == MSG_ACK) {
		if (yiaddr) offer_ip     = yiaddr;
		if (mask)   offer_mask   = mask;
		if (gw)     offer_gw     = gw;
		if (server) offer_server = server;
	}
	return msg;
}

/* Send `send_msg`, then consume frames until `want` arrives or the
 * I_SRDTMO read timeout fires.  Retries the send 3 times. */
static int exchange(int fd, uint8_t send_msg, int want)
{
	uint8_t frame[ETH_HDR_LEN + IPV4_HDR_LEN + UDP_HDR_LEN + 312];
	uint8_t rbuf[1600];

	for (int attempt = 0; attempt < 3; attempt++) {
		unsigned n = build_dhcp(frame, send_msg,
					offer_ip, offer_server);
		if (write(fd, frame, n) < 0) return -1;
		for (;;) {
			ssize_t r = read(fd, rbuf, sizeof(rbuf));
			if (r <= 0) break;	/* timeout -> resend */
			if (parse_reply(rbuf, (unsigned)r) == want)
				return 0;
		}
	}
	return -1;
}

static int plumb(const char *ifname,
		 uint32_t ip, uint32_t mask, uint32_t gw)
{
	int fd = open("/dev/udp", 0);
	if (fd < 0) return -1;
	struct kifreq ifr;
	int rc = 0;

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, ifname);
	ifr.ifr_addr = ip;
	if (ioctl(fd, SIOCSIFADDR, (long)&ifr) < 0)    rc = -1;
	ifr.ifr_addr = mask;
	if (ioctl(fd, SIOCSIFNETMASK, (long)&ifr) < 0) rc = -1;
	ifr.ifr_addr = gw;
	if (ioctl(fd, SIOCSIFGW, (long)&ifr) < 0)      rc = -1;
	if (offer_dns) {
		ifr.ifr_addr = offer_dns;
		(void)ioctl(fd, SIOCSIFDNS, (long)&ifr);
	}
	close(fd);
	return rc;
}

int main(int argc, char **argv)
{
	const char *ifname = (argc > 1) ? argv[1] : "eth0";
	char dev[32];

	strcpy(dev, "/dev/");
	strcat(dev, ifname);

	int fd = open(dev, 0);
	if (fd < 0) {
		/* No datalink (e.g. lo0-only rig): nothing to do. */
		return 0;
	}

	/* DL_INFO: learn our MAC. */
	{
		struct dl_info_req req = { .dl_primitive = DL_INFO_REQ };
		struct dl_info_ack ack;
		struct strbuf ctl = { .len = sizeof(req), .buf = &req };
		if (putmsg(fd, &ctl, 0, 0) < 0) { close(fd); return 1; }
		ctl.maxlen = sizeof(ack); ctl.buf = &ack;
		struct strbuf dat = { .maxlen = 0, .buf = 0 };
		int fl = 0;
		if (getmsg(fd, &ctl, &dat, &fl) < 0 ||
		    ack.dl_primitive != DL_INFO_ACK) {
			close(fd);
			return 1;
		}
		memcpy(our_mac, ack.dl_mac, 6);
	}

	/* DL_BIND: IPv4 frames only. */
	{
		struct dl_bind_req req = {
			.dl_primitive = DL_BIND_REQ,
			.dl_sap       = ETHERTYPE_IPV4,
		};
		struct dl_bind_ack ack;
		struct strbuf ctl = { .len = sizeof(req), .buf = &req };
		if (putmsg(fd, &ctl, 0, 0) < 0) { close(fd); return 1; }
		ctl.maxlen = sizeof(ack); ctl.buf = &ack;
		struct strbuf dat = { .maxlen = 0, .buf = 0 };
		int fl = 0;
		if (getmsg(fd, &ctl, &dat, &fl) < 0 ||
		    ack.dl_primitive != DL_BIND_ACK) {
			close(fd);
			return 1;
		}
	}

	/* 2 s read timeout drives the retransmit loop. */
	ioctl(fd, I_SRDTMO, 2000);

	int bound = 0;
	if (exchange(fd, MSG_DISCOVER, MSG_OFFER) == 0 &&
	    exchange(fd, MSG_REQUEST,  MSG_ACK)   == 0)
		bound = 1;
	close(fd);

	if (!bound) {
		/* QEMU slirp fallback -- static, well-known. */
		offer_ip   = 0x0a00020fu;	/* 10.0.2.15 */
		offer_mask = 0xffffff00u;
		offer_gw   = 0x0a000202u;	/* 10.0.2.2  */
		offer_dns  = 0x0a000203u;	/* 10.0.2.3 (slirp DNS) */
		printf("dhcpagent: no DHCP -- fallback 10.0.2.15/24\n");
	}
	if (!offer_mask) offer_mask = 0xffffff00u;

	if (plumb(ifname, offer_ip, offer_mask, offer_gw) < 0) {
		printf("dhcpagent: plumb failed\n");
		return 1;
	}
	unsigned nbits = 0;
	for (uint32_t m = offer_mask; m; m &= m - 1) nbits++;
	printf("dhcpagent: %s bound %u.%u.%u.%u/%u gw %u.%u.%u.%u%s\n",
	       ifname,
	       (offer_ip >> 24) & 0xff, (offer_ip >> 16) & 0xff,
	       (offer_ip >>  8) & 0xff,  offer_ip        & 0xff,
	       nbits,
	       (offer_gw >> 24) & 0xff, (offer_gw >> 16) & 0xff,
	       (offer_gw >>  8) & 0xff,  offer_gw        & 0xff,
	       bound ? "" : " (fallback)");
	return 0;
}

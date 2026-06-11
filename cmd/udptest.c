/*
 * cmd/udptest.c -- exercise /dev/udp via the TPI putmsg/getmsg ABI.
 *
 * Opens /dev/udp, binds a local port via T_BIND_REQ, sends a
 * datagram to the same port on loopback via T_UNITDATA_REQ, then
 * pulls the echo back via T_UNITDATA_IND.  Verifies the payload
 * round-tripped intact.
 *
 * SVR4 ABI: putmsg(fd, ctl=M_PROTO, data=M_DATA, flags) hands the
 * tagged-union primitive header in `ctl` and the user payload in
 * `data` to the udp module.  getmsg pulls a (ctl, data) pair back
 * out the stream head.
 */

#include <stdint.h>
#include <stdio.h>
#include <stropts.h>
#include <string.h>
#include <unistd.h>
#include "kappara/net/udp.h"

static int bind_port(int fd, uint16_t port)
{
	struct t_bind_req req;
	req.prim   = T_BIND_REQ;
	req._pad[0] = 0;
	req.port   = port;
	struct strbuf ctl = { .maxlen = 0, .len = sizeof(req), .buf = &req };
	if (putmsg(fd, &ctl, NULL, 0) < 0) {
		puts("udptest: putmsg T_BIND_REQ failed");
		return -1;
	}
	struct t_bind_ack ack;
	struct strbuf ctlin = {
		.maxlen = sizeof(ack), .len = 0, .buf = &ack,
	};
	int flags = 0;
	if (getmsg(fd, &ctlin, NULL, &flags) < 0) {
		puts("udptest: getmsg bind reply failed");
		return -1;
	}
	if (ctlin.len < (int)sizeof(ack) || ack.prim != T_BIND_ACK) {
		puts("udptest: bind NAK");
		return -1;
	}
	return ack.port;
}

static int send_datagram(int fd, uint32_t dst_ip, uint16_t dst_port,
                         const void *payload, unsigned len)
{
	struct t_unitdata_req req;
	req.prim     = T_UNITDATA_REQ;
	req._pad[0]  = req._pad[1] = req._pad[2] = 0;
	req.dst_ip   = dst_ip;
	req.dst_port = dst_port;
	req._pad2    = 0;
	struct strbuf ctl = {
		.maxlen = 0, .len = sizeof(req), .buf = &req,
	};
	struct strbuf data = {
		.maxlen = 0, .len = (int)len, .buf = (void *)payload,
	};
	if (putmsg(fd, &ctl, &data, 0) < 0) {
		puts("udptest: putmsg T_UNITDATA_REQ failed");
		return -1;
	}
	return 0;
}

static int recv_datagram(int fd, uint32_t *src_ip, uint16_t *src_port,
                         void *payload, unsigned cap)
{
	struct t_unitdata_ind ind;
	struct strbuf ctl = {
		.maxlen = sizeof(ind), .len = 0, .buf = &ind,
	};
	struct strbuf data = {
		.maxlen = (int)cap, .len = 0, .buf = payload,
	};
	int flags = 0;
	if (getmsg(fd, &ctl, &data, &flags) < 0) {
		puts("udptest: getmsg failed");
		return -1;
	}
	if (ctl.len < (int)sizeof(ind) || ind.prim != T_UNITDATA_IND) {
		puts("udptest: expected T_UNITDATA_IND");
		return -1;
	}
	*src_ip   = ind.src_ip;
	*src_port = ind.src_port;
	return data.len;
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	int fd = open("/dev/udp", 2);	/* O_RDWR */
	if (fd < 0) {
		puts("udptest: cannot open /dev/udp");
		return 1;
	}

	int bound = bind_port(fd, 12345);
	if (bound < 0) {
		close(fd);
		return 1;
	}
	printf("udptest: bound port %d\n", bound);

	const char payload[] = "hello udp";
	const unsigned plen = sizeof(payload) - 1;

	if (send_datagram(fd, 0x7f000001u, (uint16_t)bound,
	                  payload, plen) < 0) {
		close(fd);
		return 1;
	}

	char buf[256];
	uint32_t src_ip = 0;
	uint16_t src_port = 0;
	int n = recv_datagram(fd, &src_ip, &src_port, buf, sizeof(buf));
	if (n < 0) { close(fd); return 1; }

	printf("udptest: got %d bytes from %u.%u.%u.%u:%u\n",
	       n,
	       (src_ip >> 24) & 0xff, (src_ip >> 16) & 0xff,
	       (src_ip >>  8) & 0xff,  src_ip        & 0xff,
	       (unsigned)src_port);

	int ok = ((unsigned)n == plen);
	if (ok) {
		for (unsigned i = 0; i < plen; i++)
			if (buf[i] != payload[i]) { ok = 0; break; }
	}
	puts(ok ? "udptest: PASS" : "udptest: FAIL");
	close(fd);
	return ok ? 0 : 1;
}

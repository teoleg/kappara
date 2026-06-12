/*
 * cmd/tcptest.c -- end-to-end exercise of /dev/tcp via the TPI ABI.
 *
 * Mirrors the in-kernel tcp_selftest_run from user space, going
 * through the real putmsg/getmsg syscall path.  Two /dev/tcp opens
 * (one as listener, one as client), 3-way handshake on loopback,
 * bidirectional data exchange, byte-exact payload verification.
 *
 *   1. L = open("/dev/tcp"); bind 23456
 *   2. C = open("/dev/tcp"); bind ephemeral
 *   3. C: T_CONN_REQ to 127.0.0.1:23456
 *   4. Both L and C see T_CONN_CON (handshake done)
 *   5. C sends T_DATA_REQ "hello-from-client"
 *      L drains T_DATA_IND, verifies payload bytes
 *   6. L sends T_DATA_REQ "and-from-listener"
 *      C drains T_DATA_IND, verifies payload bytes
 *
 * This is the same flow as the kernel selftest but via the syscall
 * boundary, so it covers the full vfs -> stream_putmsg / stream_getmsg
 * path on top of the protocol logic.
 */

#include <stdint.h>
#include <stdio.h>
#include <stropts.h>
#include <string.h>
#include <unistd.h>
#include "kappara/net/tcp.h"

static int tcp_bind(int fd, uint16_t port, uint16_t *out_port)
{
	struct t_tcp_bind_req req = { .prim = T_TCP_BIND_REQ, .port = port };
	struct strbuf ctl = { .maxlen = 0, .len = sizeof(req), .buf = &req };
	if (putmsg(fd, &ctl, NULL, 0) < 0) return -1;

	struct t_tcp_bind_ack ack;
	struct strbuf cin = { .maxlen = sizeof(ack), .len = 0, .buf = &ack };
	int flags = 0;
	if (getmsg(fd, &cin, NULL, &flags) < 0) return -1;
	if (ack.prim != T_TCP_BIND_ACK) return -1;
	if (out_port) *out_port = ack.port;
	return 0;
}

static int tcp_listen(int fd)
{
	struct t_tcp_listen_req req = { .prim = T_TCP_LISTEN_REQ };
	struct strbuf ctl = { .maxlen = 0, .len = sizeof(req), .buf = &req };
	return putmsg(fd, &ctl, NULL, 0);
}

static int tcp_connect(int fd, uint32_t dst_ip, uint16_t dst_port)
{
	struct t_tcp_conn_req req = {
		.prim     = T_TCP_CONN_REQ,
		.dst_ip   = dst_ip,
		.dst_port = dst_port,
	};
	struct strbuf ctl = { .maxlen = 0, .len = sizeof(req), .buf = &req };
	return putmsg(fd, &ctl, NULL, 0);
}

static int tcp_accept(int fd)
{
	struct t_tcp_conn_res res = { .prim = T_TCP_CONN_RES };
	struct strbuf ctl = { .maxlen = 0, .len = sizeof(res), .buf = &res };
	return putmsg(fd, &ctl, NULL, 0);
}

static int await_prim(int fd, uint8_t want_prim)
{
	uint8_t buf[64];
	struct strbuf cin = { .maxlen = sizeof(buf), .len = 0, .buf = buf };
	int flags = 0;
	if (getmsg(fd, &cin, NULL, &flags) < 0) return -1;
	if (cin.len < 1 || buf[0] != want_prim) return -1;
	return 0;
}

static int await_conn_con(int fd)
{
	return await_prim(fd, T_TCP_CONN_CON);
}

static int tcp_send(int fd, const void *buf, unsigned len)
{
	struct t_tcp_data_req req = { .prim = T_TCP_DATA_REQ };
	struct strbuf ctl  = { .maxlen = 0, .len = sizeof(req), .buf = &req };
	struct strbuf data = { .maxlen = 0, .len = (int)len, .buf = (void *)buf };
	return putmsg(fd, &ctl, &data, 0);
}

static int tcp_recv_check(int fd, const char *expect, unsigned elen)
{
	struct t_tcp_data_ind ind;
	char buf[256];
	struct strbuf cin = { .maxlen = sizeof(ind), .len = 0, .buf = &ind };
	struct strbuf din = { .maxlen = sizeof(buf), .len = 0, .buf = buf };
	int flags = 0;
	if (getmsg(fd, &cin, &din, &flags) < 0) return -1;
	if (ind.prim != T_TCP_DATA_IND) return -1;
	if ((unsigned)din.len != elen) return -1;
	for (unsigned i = 0; i < elen; i++)
		if (buf[i] != expect[i]) return -1;
	return 0;
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	int L = open("/dev/tcp", 2);
	if (L < 0) { puts("tcptest: open L failed"); return 1; }
	int C = open("/dev/tcp", 2);
	if (C < 0) { puts("tcptest: open C failed"); close(L); return 1; }

	uint16_t lport = 0, cport = 0;
	if (tcp_bind(L, 23456, &lport) < 0) {
		puts("tcptest: bind L failed");
		close(L); close(C); return 1;
	}
	if (tcp_bind(C, 0, &cport) < 0) {
		puts("tcptest: bind C failed");
		close(L); close(C); return 1;
	}
	printf("tcptest: L=port %u, C=port %u\n",
	       (unsigned)lport, (unsigned)cport);

	/* L: explicit T_LISTEN_REQ.  Without it, BOUND silently drops
	 * inbound SYNs.  T1d split this from the implicit-listener
	 * behavior the earlier phases had. */
	if (tcp_listen(L) < 0) {
		puts("tcptest: T_LISTEN_REQ failed");
		close(L); close(C); return 1;
	}

	/* C: T_CONN_REQ -- sends SYN.  L will receive it, fire
	 * T_CONN_IND, and wait for the listener to send T_CONN_RES
	 * before issuing SYN-ACK. */
	if (tcp_connect(C, 0x7f000001u, lport) < 0) {
		puts("tcptest: T_CONN_REQ failed");
		close(L); close(C); return 1;
	}

	/* L: drain T_CONN_IND notification of the pending connection. */
	if (await_prim(L, T_TCP_CONN_IND) < 0) {
		puts("tcptest: L never got T_CONN_IND");
		close(L); close(C); return 1;
	}

	/* L: T_CONN_RES -- accept.  This sends SYN-ACK, which C processes
	 * inline, ACKs, transitions to ESTABLISHED + T_CONN_CON.  That ACK
	 * transitions L to ESTABLISHED + T_CONN_CON. */
	if (tcp_accept(L) < 0) {
		puts("tcptest: T_CONN_RES failed");
		close(L); close(C); return 1;
	}
	if (await_conn_con(C) < 0) {
		puts("tcptest: C never got T_CONN_CON");
		close(L); close(C); return 1;
	}
	if (await_conn_con(L) < 0) {
		puts("tcptest: L never got T_CONN_CON");
		close(L); close(C); return 1;
	}
	puts("tcptest: handshake OK, both sides ESTABLISHED");

	static const char c2l[] = "hello-from-client";
	static const char l2c[] = "and-from-listener";

	if (tcp_send(C, c2l, sizeof(c2l) - 1) < 0) {
		puts("tcptest: C T_DATA_REQ failed");
		close(L); close(C); return 1;
	}
	if (tcp_recv_check(L, c2l, sizeof(c2l) - 1) < 0) {
		puts("tcptest: L did not receive C's payload");
		close(L); close(C); return 1;
	}
	puts("tcptest: C -> L data OK");

	if (tcp_send(L, l2c, sizeof(l2c) - 1) < 0) {
		puts("tcptest: L T_DATA_REQ failed");
		close(L); close(C); return 1;
	}
	if (tcp_recv_check(C, l2c, sizeof(l2c) - 1) < 0) {
		puts("tcptest: C did not receive L's payload");
		close(L); close(C); return 1;
	}
	puts("tcptest: L -> C data OK");

	puts("tcptest: PASS");
	close(L); close(C);
	return 0;
}

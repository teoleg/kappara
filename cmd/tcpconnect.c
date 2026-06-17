/*
 * cmd/tcpconnect.c -- userland TCP dial-out via SVR4 STREAMS / TPI
 *
 * Usage:  tcpconnect <ip> <port>
 *
 * Proves that EL0 can drive the full TPI dial-out sequence -- it's
 * the EL0 mirror of what telnetd does in-kernel.  Step 1 of the
 * FTPD plan (see docs/FTPD.md): the userspace ftpd that comes
 * later opens its data sockets the same way this command does.
 *
 * Flow:
 *   open  /dev/tcp                                                     -> fd
 *   (kernel stream_open auto-pushes the "tcp" module on /dev/tcp --
 *    DON'T call I_PUSH "tcp" again or you stack two modules and
 *    handle_bind_req's IP_T_BIND_REQ loops through tcp_wq twice.)
 *   putmsg T_TCP_BIND_REQ port=0     ; getmsg expect T_TCP_BIND_ACK
 *   putmsg T_TCP_CONN_REQ ip:port    ; getmsg expect T_TCP_CONN_CON
 *   loop:
 *     read line from stdin           ; nothing to send -> ORDREL
 *     putmsg T_TCP_DATA_REQ + bytes
 *     getmsg one T_TCP_DATA_IND      ; print payload to stdout
 *   putmsg T_TCP_ORDREL_REQ
 *   getmsg expect T_TCP_ORDREL_IND
 *   close fd
 *
 * It's line-at-a-time, half duplex.  That's enough to prove the
 * TPI surface; nc-style fully-bidirectional copy needs either a
 * second thread (sys_spawn) or non-blocking getmsg, neither of
 * which is wired through libc yet.  Both can come on top of this.
 *
 * Test recipe (with kappara on virt):
 *   host$  nc -l 12345            # or `python3 -c '... echo server'`
 *   guest$ tcpconnect 10.0.2.2 12345
 *          hello              <- typed on guest, appears on host
 *   host:  echo HI                 # host responds
 *          HI                  <- guest prints
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>
#include <unistd.h>

#include "kappara/net/tcp.h"

#define BUF_SZ	256

static int parse_ipv4(const char *s, uint32_t *out)
{
	uint32_t ip = 0;
	for (int i = 3; i >= 0; i--) {
		if (*s < '0' || *s > '9') return -1;
		unsigned octet = 0;
		while (*s >= '0' && *s <= '9')
			octet = octet * 10 + (unsigned)(*s++ - '0');
		if (octet > 255) return -1;
		ip |= ((uint32_t)octet << (i * 8));
		if (i > 0) {
			if (*s != '.') return -1;
			s++;
		}
	}
	*out = ip;
	return 0;
}

static int parse_port(const char *s, uint16_t *out)
{
	unsigned p = 0;
	while (*s >= '0' && *s <= '9')
		p = p * 10 + (unsigned)(*s++ - '0');
	if (*s != '\0' || p == 0 || p > 65535) return -1;
	*out = (uint16_t)p;
	return 0;
}

/* Send one TPI primitive (with no data payload) and discard whatever
 * comes back synchronously. */
static int put_prim(int fd, const void *body, int blen)
{
	struct strbuf c = { .maxlen = 0, .len = blen, .buf = (void *)body };
	return putmsg(fd, &c, NULL, 0);
}

/* Block in getmsg until ANY mblk arrives.  Returns the primitive byte
 * (0 if no ctl), the control-buffer length in *clenp, and the data
 * length in *dlenp.  The control bytes go into ctlbuf; the data bytes
 * (if any) go into datbuf. */
static int get_msg(int fd,
                   void *ctlbuf, int ctlcap, int *clenp,
                   void *datbuf, int datcap, int *dlenp)
{
	struct strbuf c = { .maxlen = ctlcap, .len = 0, .buf = ctlbuf };
	struct strbuf d = { .maxlen = datcap, .len = 0, .buf = datbuf };
	int flags = 0;
	int r = getmsg(fd, &c, &d, &flags);
	if (r < 0) return -1;
	*clenp = c.len;
	*dlenp = d.len;
	return r;
}

/* Spin getmsg until we see the wanted primitive.  Treat
 * T_TCP_DISCON_IND as a permanent failure. */
static int wait_prim(int fd, uint8_t want)
{
	for (;;) {
		uint8_t ctl[16];
		char    dat[BUF_SZ];
		int     clen = 0, dlen = 0;
		if (get_msg(fd, ctl, sizeof(ctl), &clen,
		            dat, sizeof(dat), &dlen) < 0)
			return -1;
		uint8_t prim = clen > 0 ? ctl[0] : 0;
		if (prim == want)               return 0;
		if (prim == T_TCP_DISCON_IND)   return -1;
		/* Unexpected primitives are noise -- skip and keep
		 * waiting.  Real code would log them. */
	}
}

/* Send a chunk of user data as T_TCP_DATA_REQ + payload mblk. */
static int send_data(int fd, const void *buf, int blen)
{
	struct t_tcp_data_req r = { .prim = T_TCP_DATA_REQ };
	struct strbuf ctl = { .maxlen = 0, .len = (int)sizeof(r), .buf = &r };
	struct strbuf dat = { .maxlen = 0, .len = blen,           .buf = (void *)buf };
	return putmsg(fd, &ctl, &dat, 0);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		puts("usage: tcpconnect <ip> <port>");
		return 1;
	}
	uint32_t dst_ip;
	uint16_t dst_port;
	if (parse_ipv4(argv[1], &dst_ip) < 0) {
		puts("tcpconnect: bad ip");
		return 1;
	}
	if (parse_port(argv[2], &dst_port) < 0) {
		puts("tcpconnect: bad port");
		return 1;
	}

	int fd = open("/dev/tcp", 2);	/* O_RDWR; kernel auto-pushes "tcp" */
	if (fd < 0) { puts("tcpconnect: open /dev/tcp failed"); return 1; }

	struct t_tcp_bind_req br = { .prim = T_TCP_BIND_REQ, .port = 0 };
	if (put_prim(fd, &br, sizeof(br)) < 0
	 || wait_prim(fd, T_TCP_BIND_ACK) < 0) {
		puts("tcpconnect: bind failed");
		close(fd);
		return 1;
	}

	struct t_tcp_conn_req cr = {
		.prim     = T_TCP_CONN_REQ,
		.dst_ip   = dst_ip,
		.dst_port = dst_port,
	};
	if (put_prim(fd, &cr, sizeof(cr)) < 0
	 || wait_prim(fd, T_TCP_CONN_CON) < 0) {
		puts("tcpconnect: connect failed");
		close(fd);
		return 1;
	}

	printf("tcpconnect: connected to %u.%u.%u.%u:%u\n",
	       (dst_ip >> 24) & 0xff, (dst_ip >> 16) & 0xff,
	       (dst_ip >>  8) & 0xff,  dst_ip        & 0xff,
	       (unsigned)dst_port);

	/* Half-duplex line loop.  Read a line from stdin, send it,
	 * then drain ONE T_TCP_DATA_IND back and print it.  Empty
	 * stdin line (just Enter) means "no more input" and we
	 * gracefully tear down. */
	char line[BUF_SZ];
	for (;;) {
		if (!fgets(line, sizeof(line), stdin)) break;
		int len = (int)strlen(line);
		if (len == 0) break;
		if (line[0] == '.' && (len == 1 || line[1] == '\n')) break;

		if (send_data(fd, line, len) < 0) {
			puts("tcpconnect: send failed");
			break;
		}

		uint8_t ctl[16];
		char    dat[BUF_SZ];
		int     clen = 0, dlen = 0;
		if (get_msg(fd, ctl, sizeof(ctl), &clen,
		            dat, sizeof(dat), &dlen) < 0) {
			puts("tcpconnect: recv failed");
			break;
		}
		uint8_t prim = clen > 0 ? ctl[0] : 0;
		if (prim == T_TCP_DATA_IND && dlen > 0) {
			write(1, dat, (size_t)dlen);
		} else if (prim == T_TCP_ORDREL_IND) {
			puts("tcpconnect: peer closed");
			break;
		} else if (prim == T_TCP_DISCON_IND) {
			puts("tcpconnect: peer reset");
			break;
		}
	}

	struct t_tcp_ordrel_req orq = { .prim = T_TCP_ORDREL_REQ };
	put_prim(fd, &orq, sizeof(orq));
	(void)wait_prim(fd, T_TCP_ORDREL_IND);
	close(fd);
	return 0;
}

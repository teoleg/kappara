/*
 * cmd/ftpd.c -- RFC 959 FTP server (passive-only, no auth)
 *
 * Step 3 of the FTPD plan (docs/FTPD.md).  Long-running EL0 daemon
 * that lets the host push binaries into /home over standard FTP.
 *
 * Architecture:
 *   - Control channel on TCP 21, accepted via the same TPI dance
 *     telnetd does in-kernel (BIND_REQ -> LISTEN_REQ -> wait
 *     CONN_IND -> open responder /dev/tcp -> CONN_RES) but driven
 *     from EL0 via putmsg/getmsg.
 *   - Per-connection state machine: read CRLF-terminated command
 *     line, dispatch, write reply.  Synchronous; only one client
 *     at a time.  Real FTP servers fork per connect; we don't
 *     have a way to keep the listener live across fork yet, so
 *     serialise.
 *   - PASV opens a SECOND listener on an ephemeral port in the
 *     PASV_PORT range, replies with the ip:port encoded
 *     RFC 959 style ("h1,h2,h3,h4,p1,p2"), accepts the client's
 *     data connection, transfers, closes.
 *   - RETR streams a file from the VFS out the data channel.
 *   - STOR reads the data channel and creates/writes /home/<name>.
 *     STOR refuses any path outside /home as a guardrail against
 *     overwriting /usr/bin while a binary is mid-exec.
 *
 * What's deliberately missing (deferred):
 *   - Real authentication.  USER/PASS take any value.
 *   - ASCII mode -- TYPE I (binary) is honored, anything else gets
 *     200 OK but the bytes go unmolested.
 *   - DELE / RNFR / RNTO / MKD / RMD.  Add as needed.
 *   - LIST does a short bare-name dump from sys_ls, not the
 *     "drwxr-xr-x  ..." Unix ls output that some clients parse.
 *
 * Test recipe:
 *   host$  ftp 127.0.0.1 2121          # via Makefile hostfwd
 *   ftp> user any                       # any password accepted
 *   ftp> cd /home
 *   ftp> binary
 *   ftp> put hello                       # uploads to /home/hello
 *   ftp> ls
 *   ftp> bye
 *   kappara:/# exec /home/hello          # run the just-uploaded ELF
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>
#include <unistd.h>

#include "kappara/net/tcp.h"

/* FTP server-side ports.  Control = 21.  PASV picks one slot in
 * [PASV_BASE..PASV_BASE+PASV_N) and binds it; the Makefile
 * hostfwd table forwards the same range from the host. */
#define CONTROL_PORT	21
#define PASV_BASE	30000
#define PASV_N		8

/* The IP that goes into the PASV reply (RFC 959 h1,h2,h3,h4).
 *
 * In QEMU user-mode networking the guest is 10.0.2.15 and the host
 * reaches us at 127.0.0.1:hostfwd_port; the data-port hostfwds NAT
 * 127.0.0.1:30000+ back to guest:30000+.  If we advertised our
 * actual 10.0.2.15 here, ftp(1) would reject the reply ("passive
 * mode address mismatch") because it doesn't match the control
 * connection's peer.  Lie with 127.0.0.1 and let the host's
 * hostfwd table do the routing.
 *
 * For a non-NAT deployment (real network) bump these to the
 * actual eth0 IP. */
#define ADV_IP_A	127
#define ADV_IP_B	0
#define ADV_IP_C	0
#define ADV_IP_D	1

#define LINE_MAX	256
/* BUF_SZ must be >= TCP MSS (1460 for Ethernet) so a full data
 * segment fits in one getmsg.  Until stream_getmsg learns to
 * put-back leftover bytes, anything past `maxlen` is dropped --
 * fine for small control replies, ruinous for STOR of an ELF.
 * 4 KB gives us slack for any reasonable MSS. */
#define BUF_SZ		4096

/* ---- tiny string helpers (no libc string state) ----------------- */

static int strceq(const char *a, const char *b)
{
	while (*a && *b) {
		char x = *a++; char y = *b++;
		if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
		if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
		if (x != y) return 0;
	}
	return *a == 0 && *b == 0;
}

static int starts_with_ci(const char *s, const char *prefix)
{
	while (*prefix) {
		char x = *s++; char y = *prefix++;
		if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
		if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
		if (x != y) return 0;
	}
	return 1;
}

static void path_join(const char *cwd, const char *arg, char *out, size_t cap)
{
	size_t i = 0;
	if (arg[0] == '/') {
		while (arg[i] && i + 1 < cap) { out[i] = arg[i]; i++; }
	} else {
		size_t j = 0;
		while (cwd[j] && i + 1 < cap) out[i++] = cwd[j++];
		if (i > 1 && i + 1 < cap) out[i++] = '/';
		size_t k = 0;
		while (arg[k] && i + 1 < cap) out[i++] = arg[k++];
	}
	out[i] = '\0';
}

/* libc doesn't export creat() / unlink() yet -- inline the SVC.
 * SYS_creat = 12 and SYS_unlink = 17, mirroring user/syscall.h. */
static long sys_creat(const char *p)
{
	register long x0 __asm__("x0") = (long)(unsigned long)p;
	register long x8 __asm__("x8") = 12;	/* SYS_creat */
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
	return x0;
}

static long sys_unlink(const char *p)
{
	register long x0 __asm__("x0") = (long)(unsigned long)p;
	register long x8 __asm__("x8") = 17;	/* SYS_unlink */
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
	return x0;
}

/* ---- TPI / STREAMS helpers ------------------------------------- */

static int put_prim(int fd, const void *body, int blen)
{
	struct strbuf c = { .maxlen = 0, .len = blen, .buf = (void *)body };
	return putmsg(fd, &c, NULL, 0);
}

static int put_prim_data(int fd, const void *body, int blen,
                         const void *data, int dlen)
{
	struct strbuf c = { .maxlen = 0, .len = blen, .buf = (void *)body };
	struct strbuf d = { .maxlen = 0, .len = dlen, .buf = (void *)data };
	return putmsg(fd, &c, &d, 0);
}

/* Returns prim byte (0 if no ctl), fills ctl_buf + dat_buf with
 * lengths in clenp / dlenp.  Blocks until a message arrives. */
static int recv_msg(int fd,
                    void *ctl_buf, int ctl_cap, int *clenp,
                    void *dat_buf, int dat_cap, int *dlenp)
{
	struct strbuf c = { .maxlen = ctl_cap, .len = 0, .buf = ctl_buf };
	struct strbuf d = { .maxlen = dat_cap, .len = 0, .buf = dat_buf };
	int flags = 0;
	int r = getmsg(fd, &c, &d, &flags);
	if (r < 0) return -1;
	*clenp = c.len < 0 ? 0 : c.len;
	*dlenp = d.len < 0 ? 0 : d.len;
	return 0;
}

static int wait_prim(int fd, uint8_t want)
{
	for (;;) {
		uint8_t ctl[16]; char dat[16];
		int clen = 0, dlen = 0;
		if (recv_msg(fd, ctl, sizeof(ctl), &clen,
		             dat, sizeof(dat), &dlen) < 0)
			return -1;
		uint8_t prim = clen > 0 ? ctl[0] : 0;
		if (prim == want)             return 0;
		if (prim == T_TCP_DISCON_IND) return -1;
	}
}

/* ---- TCP socket boilerplate ------------------------------------ */

static int open_tcp(void)
{
	return open("/dev/tcp", 2);	/* O_RDWR; auto-pushes "tcp" module */
}

static int do_bind(int fd, uint16_t port)
{
	struct t_tcp_bind_req br = { .prim = T_TCP_BIND_REQ, .port = port };
	if (put_prim(fd, &br, sizeof(br)) < 0)
		return -1;
	uint8_t ctl[16]; char dat[16];
	int clen = 0, dlen = 0;
	if (recv_msg(fd, ctl, sizeof(ctl), &clen,
	             dat, sizeof(dat), &dlen) < 0)
		return -1;
	if (clen < (int)sizeof(struct t_tcp_bind_ack)
	    || ctl[0] != T_TCP_BIND_ACK)
		return -1;
	struct t_tcp_bind_ack *a = (struct t_tcp_bind_ack *)ctl;
	return (int)a->port;
}

static int do_listen(int fd)
{
	struct t_tcp_listen_req lr = { .prim = T_TCP_LISTEN_REQ };
	return put_prim(fd, &lr, sizeof(lr));
}

/* Wait for an incoming connection on `listener_fd`, build a new
 * responder fd, and finalize the accept.  Returns the responder
 * fd or -1.  Mirrors telnetd's in-kernel accept dance, just via
 * EL0 TPI.  T_CONN_RES on the listener carries the responder's
 * fd; the kernel pops the pending SYN off the listener's backlog
 * and migrates it to the responder TCB. */
static int do_accept(int listener_fd)
{
	if (wait_prim(listener_fd, T_TCP_CONN_IND) < 0) return -1;

	int rfd = open_tcp();
	if (rfd < 0) return -1;

	struct t_tcp_conn_res cr = {
		.prim          = T_TCP_CONN_RES,
		.responding_fd = rfd,
	};
	if (put_prim(listener_fd, &cr, sizeof(cr)) < 0
	    || wait_prim(rfd, T_TCP_CONN_CON) < 0) {
		close(rfd);
		return -1;
	}
	return rfd;
}

static void do_close(int fd)
{
	struct t_tcp_ordrel_req orq = { .prim = T_TCP_ORDREL_REQ };
	put_prim(fd, &orq, sizeof(orq));
	(void)wait_prim(fd, T_TCP_ORDREL_IND);
	close(fd);
}

/* ---- Control-channel reply helper ------------------------------ */

static void reply(int fd, int code, const char *msg)
{
	char line[BUF_SZ];
	int n = snprintf(line, sizeof(line), "%d %s\r\n", code, msg);
	if (n < 0) return;
	int wrote = 0;
	while (wrote < n) {
		struct t_tcp_data_req r = { .prim = T_TCP_DATA_REQ };
		int chunk = n - wrote;
		if (chunk > 256) chunk = 256;
		if (put_prim_data(fd, &r, sizeof(r),
		                  line + wrote, chunk) < 0)
			break;
		wrote += chunk;
	}
}

/* ---- Receive one CRLF-terminated control line ------------------
 *
 * Pipelined FTP clients (lftp, ftp(1) with batch scripts) send
 * several CRLF-separated commands in one TCP segment.  recv_msg
 * delivers that as one T_TCP_DATA_IND; we must remember the bytes
 * past the first \n for the NEXT recv_line call.  Single-client
 * server, so a single static stash is enough -- if we ever go
 * multi-client this needs to live on struct session.
 */
static char    pending_buf[BUF_SZ];
static int     pending_off;
static int     pending_len;

static int recv_line(int fd, char *line, int cap)
{
	int n = 0;
	for (;;) {
		while (pending_off < pending_len && n < cap - 1) {
			char c = pending_buf[pending_off++];
			if (c == '\n') {
				if (n > 0 && line[n - 1] == '\r') n--;
				line[n] = '\0';
				return n;
			}
			line[n++] = c;
		}
		uint8_t ctl[16];
		int clen = 0, dlen = 0;
		if (recv_msg(fd, ctl, sizeof(ctl), &clen,
		             pending_buf, sizeof(pending_buf), &dlen) < 0)
			return -1;
		uint8_t prim = clen > 0 ? ctl[0] : 0;
		if (prim == T_TCP_ORDREL_IND) return 0;
		if (prim == T_TCP_DISCON_IND) return -1;
		if (prim != T_TCP_DATA_IND || dlen <= 0) {
			pending_off = pending_len = 0;
			continue;
		}
		pending_off = 0;
		pending_len = dlen;
	}
}

/* ---- Dispatch state ------------------------------------------- */

struct session {
	int   ctrl_fd;
	int   data_listener_fd;	/* -1 when no PASV pending */
	int   data_port;
	char  cwd[128];
	int   type_binary;
};

/* ---- LIST: short directory dump via sys_ls --------------------- */

static long sys_ls_bytes(const char *path, char *out, size_t cap)
{
	/* libc doesn't export ls; call the syscall directly. */
	register long x0 __asm__("x0") = (long)(unsigned long)path;
	register long x1 __asm__("x1") = (long)(unsigned long)out;
	register long x2 __asm__("x2") = (long)cap;
	register long x8 __asm__("x8") = 10;	/* SYS_ls */
	__asm__ volatile("svc #0"
	                 : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8)
	                 : "memory", "cc");
	return x0;
}

/* ---- PASV setup ----------------------------------------------- */

static void cmd_pasv(struct session *s)
{
	if (s->data_listener_fd >= 0) {
		close(s->data_listener_fd);
		s->data_listener_fd = -1;
	}

	int fd = open_tcp();
	if (fd < 0) { reply(s->ctrl_fd, 425, "Can't open data socket"); return; }

	int port = -1;
	for (int i = 0; i < PASV_N; i++) {
		port = do_bind(fd, (uint16_t)(PASV_BASE + i));
		if (port > 0) break;
	}
	if (port <= 0 || do_listen(fd) < 0) {
		close(fd);
		reply(s->ctrl_fd, 425, "Can't bind PASV port");
		return;
	}

	s->data_listener_fd = fd;
	s->data_port        = port;

	char line[BUF_SZ];
	snprintf(line, sizeof(line),
	         "Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
	         ADV_IP_A, ADV_IP_B, ADV_IP_C, ADV_IP_D,
	         (port >> 8) & 0xff, port & 0xff);
	reply(s->ctrl_fd, 227, line);
}

/* Accept the client's data connection (must follow a PASV that
 * stashed the listener fd).  Returns the data fd or -1.
 *
 * We DON'T close the PASV listener here -- the listener owns the
 * IP-level bind for the data port, and the responder child shares
 * that bind for IP delivery.  Closing the listener immediately
 * after accept tears down the IP routing entry; subsequent ACKs
 * from the peer hit a port that's no longer bound and IP drops
 * them, stalling the data channel at exactly 4 segments
 * (whatever was in flight before the listener died).
 *
 * Callers must clear data_listener_fd by hand after the transfer
 * completes (see cmd_retr / cmd_stor calling data_done). */
static int data_accept(struct session *s)
{
	if (s->data_listener_fd < 0) return -1;
	return do_accept(s->data_listener_fd);
}

/* Tear down the PASV listener after a transfer is done.  Called
 * after we've graceful-closed the data fd; at this point the
 * responder TCB is gone and no further packets are expected on
 * the listener's port, so it's safe to close. */
static void data_done(struct session *s)
{
	if (s->data_listener_fd >= 0) {
		close(s->data_listener_fd);
		s->data_listener_fd = -1;
	}
}

/* ---- LIST ------------------------------------------------------ */

static void cmd_list(struct session *s, const char *arg)
{
	char target[128];
	if (arg && *arg)
		path_join(s->cwd, arg, target, sizeof(target));
	else {
		size_t i = 0;
		while (s->cwd[i] && i + 1 < sizeof(target)) {
			target[i] = s->cwd[i]; i++;
		}
		target[i] = '\0';
	}

	reply(s->ctrl_fd, 150, "Opening data connection");
	int dfd = data_accept(s);
	if (dfd < 0) {
		reply(s->ctrl_fd, 425, "Can't open data connection");
		return;
	}

	char names[BUF_SZ];
	long n = sys_ls_bytes(target, names, sizeof(names));
	if (n < 0) {
		do_close(dfd);
		reply(s->ctrl_fd, 550, "Cannot list");
		return;
	}

	/* sys_ls returns names separated by '\n'; FTP wants CRLF.
	 * Rewrite as we send. */
	char out[BUF_SZ];
	int op = 0;
	for (long i = 0; i < n; i++) {
		char c = names[i];
		if (c == '\n') {
			if (op + 2 > (int)sizeof(out)) {
				struct t_tcp_data_req r = { .prim = T_TCP_DATA_REQ };
				put_prim_data(dfd, &r, sizeof(r), out, op);
				op = 0;
			}
			out[op++] = '\r';
			out[op++] = '\n';
		} else {
			if (op + 1 > (int)sizeof(out)) {
				struct t_tcp_data_req r = { .prim = T_TCP_DATA_REQ };
				put_prim_data(dfd, &r, sizeof(r), out, op);
				op = 0;
			}
			out[op++] = c;
		}
	}
	if (op > 0) {
		struct t_tcp_data_req r = { .prim = T_TCP_DATA_REQ };
		put_prim_data(dfd, &r, sizeof(r), out, op);
	}

	do_close(dfd);
	data_done(s);
	reply(s->ctrl_fd, 226, "Transfer complete");
}

/* ---- RETR ------------------------------------------------------ */

static void cmd_retr(struct session *s, const char *arg)
{
	if (!arg || !*arg) {
		reply(s->ctrl_fd, 501, "Missing filename");
		return;
	}
	char path[128];
	path_join(s->cwd, arg, path, sizeof(path));

	int ffd = open(path, 0);
	if (ffd < 0) {
		reply(s->ctrl_fd, 550, "Cannot open file");
		return;
	}

	reply(s->ctrl_fd, 150, "Opening data connection");
	int dfd = data_accept(s);
	if (dfd < 0) {
		close(ffd);
		reply(s->ctrl_fd, 425, "Can't open data connection");
		return;
	}

	char buf[BUF_SZ];
	long n;
	while ((n = read(ffd, buf, sizeof(buf))) > 0) {
		struct t_tcp_data_req r = { .prim = T_TCP_DATA_REQ };
		if (put_prim_data(dfd, &r, sizeof(r), buf, (int)n) < 0)
			break;
	}

	close(ffd);
	do_close(dfd);
	data_done(s);
	reply(s->ctrl_fd, 226, "Transfer complete");
}

/* ---- STOR ------------------------------------------------------ */

static int starts_with(const char *s, const char *p)
{
	while (*p) if (*s++ != *p++) return 0;
	return 1;
}

static void cmd_stor(struct session *s, const char *arg)
{
	if (!arg || !*arg) {
		reply(s->ctrl_fd, 501, "Missing filename");
		return;
	}
	char path[128];
	path_join(s->cwd, arg, path, sizeof(path));

	/* Refuse anything outside /home so STOR can't overwrite a
	 * binary mid-exec.  Belt-and-suspenders: the layout puts
	 * /usr/bin and /home on different ramdisks anyway, but the
	 * client could still target /etc, /proc, /dev, etc. */
	if (!starts_with(path, "/home/")) {
		reply(s->ctrl_fd, 553, "STOR only allowed under /home");
		return;
	}

	/* kfs doesn't have O_TRUNC plumbed through, so unlink first
	 * if the name already exists.  Ignore the result -- failure
	 * just means there was nothing to unlink. */
	(void)sys_unlink(path);

	/* sys_creat lands in /home/<name>, then sys_open it for write. */
	if (sys_creat(path) < 0) {
		reply(s->ctrl_fd, 550, "Cannot create file");
		return;
	}
	int ffd = open(path, 0);
	if (ffd < 0) {
		reply(s->ctrl_fd, 550, "Cannot open file");
		return;
	}

	reply(s->ctrl_fd, 150, "Opening data connection");
	int dfd = data_accept(s);
	if (dfd < 0) {
		close(ffd);
		reply(s->ctrl_fd, 425, "Can't open data connection");
		return;
	}

	for (;;) {
		uint8_t ctl[16]; char dat[BUF_SZ];
		int clen = 0, dlen = 0;
		if (recv_msg(dfd, ctl, sizeof(ctl), &clen,
		             dat, sizeof(dat), &dlen) < 0)
			break;
		uint8_t prim = clen > 0 ? ctl[0] : 0;
		if (prim == T_TCP_DATA_IND && dlen > 0)
			(void)write(ffd, dat, (size_t)dlen);
		else if (prim == T_TCP_ORDREL_IND) break;
		else if (prim == T_TCP_DISCON_IND) break;
	}

	close(ffd);
	close(dfd);	/* peer half-closed; their ORDREL ack already drained */
	data_done(s);
	reply(s->ctrl_fd, 226, "Transfer complete");
}

/* ---- Per-session control loop --------------------------------- */

static void run_session(int ctrl_fd)
{
	struct session s = {
		.ctrl_fd          = ctrl_fd,
		.data_listener_fd = -1,
		.data_port        = 0,
		.type_binary      = 1,
	};
	s.cwd[0] = '/'; s.cwd[1] = 'h'; s.cwd[2] = 'o';
	s.cwd[3] = 'm'; s.cwd[4] = 'e'; s.cwd[5] = '\0';
	pending_off = pending_len = 0;

	reply(ctrl_fd, 220, "kappara FTP build-v2 -- anonymous, upload to /home");

	for (;;) {
		char line[LINE_MAX];
		int n = recv_line(ctrl_fd, line, sizeof(line));
		if (n <= 0) break;

		/* Split command + optional single argument on first space. */
		char *cmd = line;
		char *arg = line;
		while (*arg && *arg != ' ') arg++;
		if (*arg) { *arg++ = '\0'; while (*arg == ' ') arg++; }

		if (strceq(cmd, "USER")) {
			reply(ctrl_fd, 331, "Any password");
		} else if (strceq(cmd, "PASS")) {
			reply(ctrl_fd, 230, "Logged in");
		} else if (strceq(cmd, "SYST")) {
			reply(ctrl_fd, 215, "UNIX Type: L8");
		} else if (strceq(cmd, "FEAT")) {
			reply(ctrl_fd, 211, "no features");
		} else if (strceq(cmd, "TYPE")) {
			s.type_binary = starts_with_ci(arg, "I");
			reply(ctrl_fd, 200, "Type set");
		} else if (strceq(cmd, "PWD") || strceq(cmd, "XPWD")) {
			char r[BUF_SZ];
			snprintf(r, sizeof(r), "\"%s\" is the current directory",
			         s.cwd);
			reply(ctrl_fd, 257, r);
		} else if (strceq(cmd, "CWD")) {
			char tgt[128];
			path_join(s.cwd, arg, tgt, sizeof(tgt));
			size_t i = 0;
			while (tgt[i] && i + 1 < sizeof(s.cwd)) {
				s.cwd[i] = tgt[i]; i++;
			}
			s.cwd[i] = '\0';
			reply(ctrl_fd, 250, "OK");
		} else if (strceq(cmd, "PASV")) {
			cmd_pasv(&s);
		} else if (strceq(cmd, "LIST") || strceq(cmd, "NLST")) {
			cmd_list(&s, arg);
		} else if (strceq(cmd, "RETR")) {
			cmd_retr(&s, arg);
		} else if (strceq(cmd, "STOR")) {
			cmd_stor(&s, arg);
		} else if (strceq(cmd, "NOOP")) {
			reply(ctrl_fd, 200, "OK");
		} else if (strceq(cmd, "QUIT")) {
			reply(ctrl_fd, 221, "Bye");
			break;
		} else {
			reply(ctrl_fd, 502, "Not implemented");
		}
	}

	if (s.data_listener_fd >= 0) close(s.data_listener_fd);
	do_close(ctrl_fd);
}

/* ---- main: bind, listen, accept loop -------------------------- */

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	int listener = open_tcp();
	if (listener < 0) { puts("ftpd: open /dev/tcp failed"); return 1; }

	int port = do_bind(listener, CONTROL_PORT);
	if (port < 0) { puts("ftpd: bind 21 failed"); close(listener); return 1; }
	if (do_listen(listener) < 0) {
		puts("ftpd: listen failed");
		close(listener);
		return 1;
	}

	puts("ftpd: listening on TCP port 21 -- uploads land under /home");

	for (;;) {
		int cfd = do_accept(listener);
		if (cfd < 0) continue;
		run_session(cfd);
	}
	/* unreachable */
}

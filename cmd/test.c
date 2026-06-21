/*
 * cmd/test -- unified selftest runner.
 *
 * One ELF, many tests.  Replaces forktest / waittest / sigtest /
 * masktest / segvtest / pipe / pipework / malloctest / udptest /
 * pktfilttest / tcptest.  Going forward, new selftests are added
 * here as a static function plus a row in `tests[]`; do not create
 * new cmd/<name>.c binaries just to exercise a feature.
 *
 * Usage:
 *   test            list available tests
 *   test all        run every test, print summary
 *   test <name>     run just the named test
 *
 * Each test function returns 0 on PASS, 1 on FAIL.  Prints its own
 * progress; main() prints a one-line summary.
 */

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>
#include <unistd.h>
#include "../user/syscall.h"
#include "kappara/net/pktfilter.h"
#include "kappara/net/tcp.h"
#include "kappara/net/udp.h"

/* ---- fork ------------------------------------------------------- */

static int shared_word = 0xAA;

static int test_fork(void)
{
	int before = shared_word;
	pid_t kid = fork();
	if (kid < 0) {
		printf("fork: FAIL fork returned %d\n", (int)kid);
		return 1;
	}
	if (kid == 0) {
		shared_word = 0xBB;
		_exit(shared_word == 0xBB ? 42 : 11);
	}
	int rc = wait((int)kid);
	if (rc != 42) {
		printf("fork: FAIL child exit=%d (want 42)\n", rc);
		return 1;
	}
	if (shared_word != before) {
		printf("fork: FAIL parent shared=%x (want %x)\n",
		       shared_word, before);
		return 1;
	}
	printf("fork: PASS (child=%d)\n", (int)kid);
	return 0;
}

/* ---- wait ------------------------------------------------------- */

static void worker_exit42(long arg)
{
	sys_exit((int)arg);
}

static int test_wait(void)
{
	long tid = sys_spawn(worker_exit42, 42);
	if (tid < 0) { puts("wait: FAIL spawn"); return 1; }
	long r = sys_wait((int)tid);
	if (r != 42) { printf("wait: FAIL got %ld want 42\n", r); return 1; }
	tid = sys_spawn(worker_exit42, 0);
	if (tid < 0) { puts("wait: FAIL spawn 2"); return 1; }
	r = sys_wait((int)tid);
	if (r != 0)  { printf("wait: FAIL got %ld want 0\n", r); return 1; }
	puts("wait: PASS");
	return 0;
}

/* ---- sig (basic signal handler delivery) ------------------------ */

static volatile int sig_marker;

static void sig_handler(int sig)
{
	(void)sig;
	sig_marker = 0xC0DE;
}

static int test_sig(void)
{
	struct sigaction sa, old;
	sa.sa_handler = sig_handler; sa.sa_mask = 0; sa.sa_flags = 0;
	if (sys_sigaction(SIGTERM, &sa, &old) < 0) {
		puts("sig: FAIL sigaction"); return 1;
	}
	sig_marker = 0;
	sys_kill((int)sys_getpid(), SIGTERM);
	int ok = (sig_marker == 0xC0DE);
	struct sigaction restore;
	restore.sa_handler = SIG_DFL_PTR;
	restore.sa_mask = 0; restore.sa_flags = 0;
	sys_sigaction(SIGTERM, &restore, 0);
	if (!ok) { puts("sig: FAIL marker not set"); return 1; }
	puts("sig: PASS");
	return 0;
}

/* ---- mask (sigprocmask + sigsuspend) ---------------------------- */

static volatile int mask_marker;

static void mask_handler(int sig)
{
	(void)sig;
	mask_marker = 1;
}

static int test_mask(void)
{
	struct sigaction sa, old_sa;
	sa.sa_handler = mask_handler;
	sa.sa_mask = 0; sa.sa_flags = 0;
	if (sys_sigaction(SIGTERM, &sa, &old_sa) < 0) {
		puts("mask: FAIL sigaction"); return 1;
	}
	unsigned int block = 1u << (SIGTERM - 1), old_mask = 0;
	sys_sigprocmask(SIG_BLOCK, &block, &old_mask);
	mask_marker = 0;
	sys_kill((int)sys_getpid(), SIGTERM);
	sys_sigsuspend(old_mask);
	int ok = mask_marker;
	sys_sigaction(SIGTERM, &old_sa, 0);
	sys_sigprocmask(SIG_SETMASK, &old_mask, 0);
	if (!ok) { puts("mask: FAIL marker"); return 1; }
	puts("mask: PASS");
	return 0;
}

/* ---- segv (SIGSEGV via NULL deref in a worker thread) ---------- */

static void segv_worker_handler(int sig)
{
	(void)sig;
	sys_log("segv: handler caught");
	sys_exit(0xCD);
}

static void segv_worker(long arg)
{
	(void)arg;
	struct sigaction sa;
	sa.sa_handler = segv_worker_handler;
	sa.sa_mask = 0; sa.sa_flags = 0;
	sys_sigaction(SIGSEGV, &sa, 0);
	volatile int *p = (volatile int *)0;
	*p = 1;
	sys_log("segv: BUG continued past fault");
	sys_exit(0);
}

static int test_segv(void)
{
	long tid = sys_spawn(segv_worker, 0);
	if (tid < 0) { puts("segv: FAIL spawn"); return 1; }
	long r = sys_wait((int)tid);
	if (r != 0xCD) {
		printf("segv: FAIL worker exit=%ld want 0xCD\n", r);
		return 1;
	}
	puts("segv: PASS");
	return 0;
}

/* ---- pipe (basic write + read round-trip) ----------------------- */

static int test_pipe(void)
{
	int fds[2];
	if (sys_pipe(fds) < 0) { puts("pipe: FAIL sys_pipe"); return 1; }
	const char *msg = "hello pipe";
	long w = write(fds[1], msg, strlen(msg));
	if (w != (long)strlen(msg)) {
		puts("pipe: FAIL write"); close(fds[0]); close(fds[1]);
		return 1;
	}
	char buf[64];
	long n = read(fds[0], buf, sizeof(buf));
	close(fds[0]); close(fds[1]);
	if (n != (long)strlen(msg)
	    || memcmp(buf, msg, strlen(msg)) != 0) {
		puts("pipe: FAIL data mismatch");
		return 1;
	}
	puts("pipe: PASS");
	return 0;
}

/* ---- pipework (two worker threads, pipe writer + reader) ------- */

static void pipe_writer(long packed)
{
	int keep = (int)(packed >> 16), discard = (int)(packed & 0xffff);
	sys_close(discard);
	const char *msg = "pipework-writer";
	sys_write(keep, msg, (size_t)strlen(msg));
	sys_close(keep);
	sys_exit(0);
}

static void pipe_reader(long packed)
{
	int keep = (int)(packed >> 16), discard = (int)(packed & 0xffff);
	sys_close(discard);
	char buf[64];
	(void)sys_read(keep, buf, sizeof(buf));	/* may be 0 if parent
						 * closed early; smoke test
						 * only verifies clean exit */
	sys_close(keep);
	sys_exit(0);
}

static int test_pipework(void)
{
	int fds[2];
	if (sys_pipe(fds) < 0) { puts("pipework: FAIL pipe"); return 1; }
	long wpacked = ((long)fds[1] << 16) | (long)(fds[0] & 0xffff);
	long rpacked = ((long)fds[0] << 16) | (long)(fds[1] & 0xffff);
	long wtid = sys_spawn(pipe_writer, wpacked);
	long rtid = sys_spawn(pipe_reader, rpacked);
	sys_close(fds[0]); sys_close(fds[1]);
	if (wtid < 0 || rtid < 0) {
		puts("pipework: FAIL spawn"); return 1;
	}
	sys_wait((int)wtid);
	sys_wait((int)rtid);
	puts("pipework: PASS");
	return 0;
}

/* ---- malloc (basic libc malloc/free/calloc round-trip) --------- */

static int test_malloc(void)
{
	char *p = malloc(64);
	if (!p) { puts("malloc: FAIL m64"); return 1; }
	for (int i = 0; i < 64; i++) p[i] = (char)i;
	for (int i = 0; i < 64; i++) {
		if (p[i] != (char)i) { puts("malloc: FAIL p"); return 1; }
	}
	char *q = malloc(128);
	if (!q) { puts("malloc: FAIL m128"); return 1; }
	if ((unsigned long)q < (unsigned long)p + 64) {
		puts("malloc: FAIL overlap"); return 1;
	}
	for (int i = 0; i < 128; i++) q[i] = (char)(i ^ 0x55);
	for (int i = 0; i < 128; i++) {
		if (q[i] != (char)(i ^ 0x55)) {
			puts("malloc: FAIL q"); return 1;
		}
	}
	free(p);
	int *arr = calloc(16, sizeof(int));
	if (!arr) { puts("malloc: FAIL calloc"); return 1; }
	for (int i = 0; i < 16; i++) {
		if (arr[i] != 0) { puts("malloc: FAIL calloc zero"); return 1; }
	}
	for (int i = 0; i < 128; i++) {
		if (q[i] != (char)(i ^ 0x55)) {
			puts("malloc: FAIL q corrupted"); return 1;
		}
	}
	puts("malloc: PASS");
	return 0;
}

/* ---- udp (TPI round-trip via /dev/udp) -------------------------- */

static int udp_bind(int fd, uint16_t port)
{
	struct t_bind_req req = { .prim = T_BIND_REQ, .port = port };
	struct strbuf ctl = { .maxlen = 0, .len = sizeof(req), .buf = &req };
	if (putmsg(fd, &ctl, NULL, 0) < 0) return -1;
	struct t_bind_ack ack;
	struct strbuf cin = { .maxlen = sizeof(ack), .len = 0, .buf = &ack };
	int flags = 0;
	if (getmsg(fd, &cin, NULL, &flags) < 0) return -1;
	return ack.prim == T_BIND_ACK ? 0 : -1;
}

static int test_udp(void)
{
	int fd = open("/dev/udp", 2);
	if (fd < 0) { puts("udp: FAIL open"); return 1; }
	if (udp_bind(fd, 12345) < 0) { puts("udp: FAIL bind"); close(fd); return 1; }
	struct t_unitdata_req req = {
		.prim = T_UNITDATA_REQ,
		.dst_ip = 0x7f000001u, .dst_port = 12345,
	};
	const char payload[] = "udp-test";
	struct strbuf ctl  = { .maxlen = 0, .len = sizeof(req), .buf = &req };
	struct strbuf data = { .maxlen = 0, .len = sizeof(payload) - 1,
	                       .buf = (void *)payload };
	if (putmsg(fd, &ctl, &data, 0) < 0) {
		puts("udp: FAIL send"); close(fd); return 1;
	}
	struct t_unitdata_ind ind;
	char buf[64];
	struct strbuf cin = { .maxlen = sizeof(ind), .len = 0, .buf = &ind };
	struct strbuf din = { .maxlen = sizeof(buf), .len = 0, .buf = buf };
	int flags = 0;
	if (getmsg(fd, &cin, &din, &flags) < 0
	    || din.len != (int)(sizeof(payload) - 1)
	    || memcmp(buf, payload, sizeof(payload) - 1) != 0) {
		puts("udp: FAIL recv"); close(fd); return 1;
	}
	close(fd);
	puts("udp: PASS");
	return 0;
}

/* ---- pktfilt (push/configure/pop the pktfilter module) --------- */

static int test_pktfilt(void)
{
	int fd = open("/dev/udp", 2);
	if (fd < 0) { puts("pktfilt: FAIL open"); return 1; }
	if (udp_bind(fd, 23001) < 0) { puts("pktfilt: FAIL bind"); close(fd); return 1; }

	const char payload[] = "drop-me";
	struct t_unitdata_req sreq = {
		.prim = T_UNITDATA_REQ,
		.dst_ip = 0x7f000001u, .dst_port = 23001,
	};
	struct strbuf sctl = { .maxlen = 0, .len = sizeof(sreq), .buf = &sreq };
	struct strbuf sdat = { .maxlen = 0, .len = sizeof(payload) - 1,
	                       .buf = (void *)payload };

	/* Baseline: send + receive. */
	if (putmsg(fd, &sctl, &sdat, 0) < 0) {
		puts("pktfilt: FAIL baseline send"); close(fd); return 1;
	}
	struct t_unitdata_ind ind;
	char buf[64];
	struct strbuf rcin = { .maxlen = sizeof(ind), .len = 0, .buf = &ind };
	struct strbuf rdin = { .maxlen = sizeof(buf), .len = 0, .buf = buf };
	int flags = 0;
	if (getmsg(fd, &rcin, &rdin, &flags) < 0
	    || rdin.len != (int)(sizeof(payload) - 1)) {
		puts("pktfilt: FAIL baseline recv"); close(fd); return 1;
	}

	/* Push pktfilter. */
	if (ioctl(fd, I_PUSH, (long)(unsigned long)"pktfilter") < 0) {
		puts("pktfilt: FAIL I_PUSH"); close(fd); return 1;
	}

	/* Configure: drop dst_port=23001. */
	struct pf_set_req creq = {
		.prim = PF_T_SET_REQ, .enabled = 1,
		.drop_dst_ip = 0, .drop_dst_port = 23001,
	};
	struct strbuf cctl = { .maxlen = 0, .len = sizeof(creq), .buf = &creq };
	if (putmsg(fd, &cctl, NULL, 0) < 0) {
		puts("pktfilt: FAIL set req"); close(fd); return 1;
	}
	struct pf_set_ack cack;
	struct strbuf rcin2 = { .maxlen = sizeof(cack), .len = 0, .buf = &cack };
	if (getmsg(fd, &rcin2, NULL, &flags) < 0
	    || cack.prim != PF_T_SET_ACK) {
		puts("pktfilt: FAIL set ack"); close(fd); return 1;
	}

	/* Send -- should be dropped. */
	if (putmsg(fd, &sctl, &sdat, 0) < 0) {
		puts("pktfilt: FAIL drop send"); close(fd); return 1;
	}
	struct pf_stats_req qreq = { .prim = PF_T_STATS_REQ };
	struct strbuf qctl = { .maxlen = 0, .len = sizeof(qreq), .buf = &qreq };
	if (putmsg(fd, &qctl, NULL, 0) < 0) {
		puts("pktfilt: FAIL stats req"); close(fd); return 1;
	}
	struct pf_stats_res qres;
	struct strbuf rqcin = { .maxlen = sizeof(qres), .len = 0, .buf = &qres };
	if (getmsg(fd, &rqcin, NULL, &flags) < 0
	    || qres.prim != PF_T_STATS_RES) {
		puts("pktfilt: FAIL stats res"); close(fd); return 1;
	}
	if (qres.dropped != 1) {
		printf("pktfilt: FAIL dropped=%u want 1\n",
		       (unsigned)qres.dropped);
		close(fd); return 1;
	}

	/* Pop and verify traffic flows again. */
	if (ioctl(fd, I_POP, 0) < 0) {
		puts("pktfilt: FAIL I_POP"); close(fd); return 1;
	}
	if (putmsg(fd, &sctl, &sdat, 0) < 0) {
		puts("pktfilt: FAIL post-pop send"); close(fd); return 1;
	}
	rdin.len = 0;
	rcin.len = 0;
	if (getmsg(fd, &rcin, &rdin, &flags) < 0
	    || rdin.len != (int)(sizeof(payload) - 1)) {
		puts("pktfilt: FAIL post-pop recv"); close(fd); return 1;
	}
	close(fd);
	puts("pktfilt: PASS");
	return 0;
}

/* ---- tcp (LISTEN + accept + handshake + data + close) ---------- */

static int tcp_bind_(int fd, uint16_t port, uint16_t *out_port)
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

static int tcp_put_prim(int fd, const void *body, unsigned blen)
{
	struct strbuf ctl = { .maxlen = 0, .len = (int)blen, .buf = (void *)body };
	return putmsg(fd, &ctl, NULL, 0);
}

static int tcp_await_prim(int fd, uint8_t want)
{
	uint8_t buf[64];
	struct strbuf cin = { .maxlen = sizeof(buf), .len = 0, .buf = buf };
	int flags = 0;
	if (getmsg(fd, &cin, NULL, &flags) < 0) return -1;
	return (cin.len >= 1 && buf[0] == want) ? 0 : -1;
}

static int test_tcp(void)
{
	int L = open("/dev/tcp", 2);
	int C = open("/dev/tcp", 2);
	int R = open("/dev/tcp", 2);
	if (L < 0 || C < 0 || R < 0) { puts("tcp: FAIL open"); goto fail_close; }

	uint16_t lport = 0;
	if (tcp_bind_(L, 23456, &lport) < 0
	    || tcp_bind_(C, 0, NULL) < 0) {
		puts("tcp: FAIL bind"); goto fail_close;
	}

	struct t_tcp_listen_req lreq = { .prim = T_TCP_LISTEN_REQ };
	if (tcp_put_prim(L, &lreq, sizeof(lreq)) < 0) {
		puts("tcp: FAIL listen"); goto fail_close;
	}

	struct t_tcp_conn_req creq = {
		.prim = T_TCP_CONN_REQ, .dst_ip = 0x7f000001u, .dst_port = lport,
	};
	if (tcp_put_prim(C, &creq, sizeof(creq)) < 0) {
		puts("tcp: FAIL connect"); goto fail_close;
	}

	if (tcp_await_prim(L, T_TCP_CONN_IND) < 0) {
		puts("tcp: FAIL CONN_IND"); goto fail_close;
	}

	/* Accept onto R (the responding endpoint).  L stays in LISTEN. */
	struct t_tcp_conn_res cres = {
		.prim = T_TCP_CONN_RES, .responding_fd = R,
	};
	if (tcp_put_prim(L, &cres, sizeof(cres)) < 0) {
		puts("tcp: FAIL accept"); goto fail_close;
	}
	if (tcp_await_prim(C, T_TCP_CONN_CON) < 0
	    || tcp_await_prim(R, T_TCP_CONN_CON) < 0) {
		puts("tcp: FAIL CONN_CON"); goto fail_close;
	}

	/* Data round-trip both ways. */
	static const char c2r[] = "ping";
	static const char r2c[] = "pong";
	struct t_tcp_data_req dreq = { .prim = T_TCP_DATA_REQ };
	struct strbuf dctl = { .maxlen = 0, .len = sizeof(dreq), .buf = &dreq };
	struct strbuf c2rdat = { .maxlen = 0, .len = sizeof(c2r) - 1,
	                         .buf = (void *)c2r };
	if (putmsg(C, &dctl, &c2rdat, 0) < 0) {
		puts("tcp: FAIL C->R send"); goto fail_close;
	}
	{
		uint8_t ctlbuf[16]; char db[32];
		struct strbuf rcin = { .maxlen = sizeof(ctlbuf), .len = 0, .buf = ctlbuf };
		struct strbuf rdin = { .maxlen = sizeof(db), .len = 0, .buf = db };
		int flags = 0;
		if (getmsg(R, &rcin, &rdin, &flags) < 0
		    || ctlbuf[0] != T_TCP_DATA_IND
		    || rdin.len != (int)(sizeof(c2r) - 1)
		    || memcmp(db, c2r, sizeof(c2r) - 1) != 0) {
			puts("tcp: FAIL C->R recv"); goto fail_close;
		}
	}
	struct strbuf r2cdat = { .maxlen = 0, .len = sizeof(r2c) - 1,
	                         .buf = (void *)r2c };
	if (putmsg(R, &dctl, &r2cdat, 0) < 0) {
		puts("tcp: FAIL R->C send"); goto fail_close;
	}
	{
		uint8_t ctlbuf[16]; char db[32];
		struct strbuf rcin = { .maxlen = sizeof(ctlbuf), .len = 0, .buf = ctlbuf };
		struct strbuf rdin = { .maxlen = sizeof(db), .len = 0, .buf = db };
		int flags = 0;
		if (getmsg(C, &rcin, &rdin, &flags) < 0
		    || ctlbuf[0] != T_TCP_DATA_IND
		    || rdin.len != (int)(sizeof(r2c) - 1)
		    || memcmp(db, r2c, sizeof(r2c) - 1) != 0) {
			puts("tcp: FAIL R->C recv"); goto fail_close;
		}
	}

	/* Graceful close of the accepted connection. */
	struct t_tcp_ordrel_req oreq = { .prim = T_TCP_ORDREL_REQ };
	if (tcp_put_prim(C, &oreq, sizeof(oreq)) < 0
	 || tcp_await_prim(R, T_TCP_ORDREL_IND) < 0
	 || tcp_put_prim(R, &oreq, sizeof(oreq)) < 0
	 || tcp_await_prim(C, T_TCP_ORDREL_IND) < 0) {
		puts("tcp: FAIL ORDREL"); goto fail_close;
	}

	close(L); close(C); close(R);
	puts("tcp: PASS");
	return 0;

fail_close:
	if (L >= 0) close(L);
	if (C >= 0) close(C);
	if (R >= 0) close(R);
	return 1;
}

/* ---- tcp multi-accept: two concurrent clients onto one listener ---- */
/* Proves the listener stays in LISTEN across accepts: C1's SYN arrives
 * while C2's SYN is also in flight, both get queued, both get accepted
 * onto fresh responder fds, and both round-trip independently. */
static int test_tcpmulti(void)
{
	int L = -1, C1 = -1, C2 = -1, R1 = -1, R2 = -1;
	L  = open("/dev/tcp", 2);
	C1 = open("/dev/tcp", 2);
	C2 = open("/dev/tcp", 2);
	R1 = open("/dev/tcp", 2);
	R2 = open("/dev/tcp", 2);
	if (L < 0 || C1 < 0 || C2 < 0 || R1 < 0 || R2 < 0) {
		puts("tcpmulti: FAIL open"); goto fail;
	}

	uint16_t lport = 0;
	if (tcp_bind_(L, 24680, &lport) < 0
	 || tcp_bind_(C1, 0, NULL) < 0
	 || tcp_bind_(C2, 0, NULL) < 0) {
		puts("tcpmulti: FAIL bind"); goto fail;
	}

	struct t_tcp_listen_req lreq = { .prim = T_TCP_LISTEN_REQ };
	if (tcp_put_prim(L, &lreq, sizeof(lreq)) < 0) {
		puts("tcpmulti: FAIL listen"); goto fail;
	}

	/* Both clients connect.  Lo0 is synchronous so each SYN runs to
	 * completion (queued at L) before the next connect call begins. */
	struct t_tcp_conn_req creq = {
		.prim = T_TCP_CONN_REQ, .dst_ip = 0x7f000001u, .dst_port = lport,
	};
	if (tcp_put_prim(C1, &creq, sizeof(creq)) < 0
	 || tcp_put_prim(C2, &creq, sizeof(creq)) < 0) {
		puts("tcpmulti: FAIL connect"); goto fail;
	}

	/* L sees TWO CONN_INDs and is still in LISTEN. */
	if (tcp_await_prim(L, T_TCP_CONN_IND) < 0
	 || tcp_await_prim(L, T_TCP_CONN_IND) < 0) {
		puts("tcpmulti: FAIL CONN_IND x2"); goto fail;
	}

	/* Accept each onto its own responder. */
	struct t_tcp_conn_res cres1 = {
		.prim = T_TCP_CONN_RES, .responding_fd = R1,
	};
	struct t_tcp_conn_res cres2 = {
		.prim = T_TCP_CONN_RES, .responding_fd = R2,
	};
	if (tcp_put_prim(L, &cres1, sizeof(cres1)) < 0
	 || tcp_put_prim(L, &cres2, sizeof(cres2)) < 0) {
		puts("tcpmulti: FAIL CONN_RES x2"); goto fail;
	}

	/* Each client + each responder sees its own CONN_CON.  The
	 * pairing depends on FIFO order in the pending queue -- the
	 * first SYN's accept lands on the first responder. */
	if (tcp_await_prim(C1, T_TCP_CONN_CON) < 0
	 || tcp_await_prim(C2, T_TCP_CONN_CON) < 0
	 || tcp_await_prim(R1, T_TCP_CONN_CON) < 0
	 || tcp_await_prim(R2, T_TCP_CONN_CON) < 0) {
		puts("tcpmulti: FAIL CONN_CON x4"); goto fail;
	}

	/* Cross-traffic on both connections to make sure the segments
	 * route to the right responder (not interleaved). */
	struct t_tcp_data_req dreq = { .prim = T_TCP_DATA_REQ };
	struct strbuf dctl = { .maxlen = 0, .len = sizeof(dreq), .buf = &dreq };
	static const char m1[] = "AAA";
	static const char m2[] = "BBB";
	struct strbuf d1 = { .maxlen = 0, .len = sizeof(m1) - 1, .buf = (void*)m1 };
	struct strbuf d2 = { .maxlen = 0, .len = sizeof(m2) - 1, .buf = (void*)m2 };
	if (putmsg(C1, &dctl, &d1, 0) < 0 || putmsg(C2, &dctl, &d2, 0) < 0) {
		puts("tcpmulti: FAIL data send"); goto fail;
	}
	for (int i = 0; i < 2; i++) {
		uint8_t ctlbuf[16]; char db[8];
		struct strbuf rcin = { .maxlen = sizeof(ctlbuf), .len = 0, .buf = ctlbuf };
		struct strbuf rdin = { .maxlen = sizeof(db), .len = 0, .buf = db };
		int flags = 0;
		int fd = i ? R2 : R1;
		const char *want = i ? m2 : m1;
		if (getmsg(fd, &rcin, &rdin, &flags) < 0
		    || ctlbuf[0] != T_TCP_DATA_IND
		    || rdin.len != 3 || memcmp(db, want, 3) != 0) {
			puts("tcpmulti: FAIL data recv"); goto fail;
		}
	}

	close(L); close(C1); close(C2); close(R1); close(R2);
	puts("tcpmulti: PASS");
	return 0;
fail:
	if (L  >= 0) close(L);
	if (C1 >= 0) close(C1);
	if (C2 >= 0) close(C2);
	if (R1 >= 0) close(R1);
	if (R2 >= 0) close(R2);
	return 1;
}

/* ---- tcp bulk transfer: exercises cwnd slow-start segmentation ---- */
/* The standard `test tcp` round-trips 4 bytes, well under one MSS, so
 * congestion control never kicks in.  This one ships 2000 bytes
 * (capped under the largest kmalloc size cache of 2048) which
 * still forces ~4 MSS-sized segments + ACK feedback to grow cwnd
 * from IW=2*MSS up through slow start.  Verifies bytes arrive in
 * order and intact. */
#define TCPBIG_LEN	2000

static int test_tcpbig(void)
{
	int L = -1, C = -1, R = -1;
	L = open("/dev/tcp", 2);
	C = open("/dev/tcp", 2);
	R = open("/dev/tcp", 2);
	if (L < 0 || C < 0 || R < 0) { puts("tcpbig: FAIL open"); goto fail; }

	uint16_t lport = 0;
	if (tcp_bind_(L, 25000, &lport) < 0 || tcp_bind_(C, 0, NULL) < 0) {
		puts("tcpbig: FAIL bind"); goto fail;
	}
	struct t_tcp_listen_req lreq = { .prim = T_TCP_LISTEN_REQ };
	if (tcp_put_prim(L, &lreq, sizeof(lreq)) < 0) {
		puts("tcpbig: FAIL listen"); goto fail;
	}
	struct t_tcp_conn_req creq = {
		.prim = T_TCP_CONN_REQ, .dst_ip = 0x7f000001u, .dst_port = lport,
	};
	if (tcp_put_prim(C, &creq, sizeof(creq)) < 0
	 || tcp_await_prim(L, T_TCP_CONN_IND) < 0) {
		puts("tcpbig: FAIL CONN_IND"); goto fail;
	}
	struct t_tcp_conn_res cres = {
		.prim = T_TCP_CONN_RES, .responding_fd = R,
	};
	if (tcp_put_prim(L, &cres, sizeof(cres)) < 0
	 || tcp_await_prim(C, T_TCP_CONN_CON) < 0
	 || tcp_await_prim(R, T_TCP_CONN_CON) < 0) {
		puts("tcpbig: FAIL handshake"); goto fail;
	}

	/* Fill a buffer with a verifiable pattern. */
	static char src[TCPBIG_LEN];
	for (int i = 0; i < TCPBIG_LEN; i++)
		src[i] = (char)(i & 0xff);

	struct t_tcp_data_req dreq = { .prim = T_TCP_DATA_REQ };
	struct strbuf dctl = { .maxlen = 0, .len = sizeof(dreq), .buf = &dreq };
	struct strbuf ddat = { .maxlen = 0, .len = TCPBIG_LEN, .buf = src };
	if (putmsg(C, &dctl, &ddat, 0) < 0) {
		puts("tcpbig: FAIL send"); goto fail;
	}

	/* Drain T_DATA_INDs off R until we've collected TCPBIG_LEN
	 * bytes.  cwnd-driven segmentation means R gets several
	 * smaller indications instead of one big one. */
	static char rcv[TCPBIG_LEN];
	int got = 0;
	while (got < TCPBIG_LEN) {
		uint8_t ctlbuf[16];
		struct strbuf rcin = { .maxlen = sizeof(ctlbuf), .len = 0, .buf = ctlbuf };
		struct strbuf rdin = { .maxlen = TCPBIG_LEN - got, .len = 0,
		                       .buf = rcv + got };
		int flags = 0;
		if (getmsg(R, &rcin, &rdin, &flags) < 0
		    || ctlbuf[0] != T_TCP_DATA_IND
		    || rdin.len <= 0) {
			puts("tcpbig: FAIL recv"); goto fail;
		}
		got += rdin.len;
	}
	if (memcmp(src, rcv, TCPBIG_LEN) != 0) {
		puts("tcpbig: FAIL data mismatch"); goto fail;
	}

	struct t_tcp_ordrel_req oreq = { .prim = T_TCP_ORDREL_REQ };
	if (tcp_put_prim(C, &oreq, sizeof(oreq)) < 0
	 || tcp_await_prim(R, T_TCP_ORDREL_IND) < 0
	 || tcp_put_prim(R, &oreq, sizeof(oreq)) < 0
	 || tcp_await_prim(C, T_TCP_ORDREL_IND) < 0) {
		puts("tcpbig: FAIL ordrel"); goto fail;
	}

	close(L); close(C); close(R);
	printf("tcpbig: PASS (%d bytes)\n", TCPBIG_LEN);
	return 0;
fail:
	if (L >= 0) close(L);
	if (C >= 0) close(C);
	if (R >= 0) close(R);
	return 1;
}

/* ---- registry --------------------------------------------------- */

struct test_entry { const char *name; int (*fn)(void); };

/* DYNAMIC.md stage 7: dlopen/dlsym round-trip.
 *
 * Loads /lib/libdltest.so (a tiny self-contained .so shipped in the
 * kernel image), resolves "dltest_answer" + "dltest_get_label",
 * calls them, checks both that the function returned 42 and that
 * the string pointer (which goes through R_AARCH64_RELATIVE) matches
 * "dltest". */
static int test_dl(void)
{
	void *h = dlopen("/lib/libdltest.so", 0);
	if (!h) { puts("dl: dlopen failed"); return 1; }

	int (*answer)(void) = (int (*)(void))dlsym(h, "dltest_answer");
	if (!answer) { puts("dl: dlsym dltest_answer failed"); return 1; }
	int r = answer();
	if (r != 42) { printf("dl: answer returned %d, want 42\n", r); return 1; }

	const char *(*get_label)(void) =
		(const char *(*)(void))dlsym(h, "dltest_get_label");
	if (!get_label) { puts("dl: dlsym dltest_get_label failed"); return 1; }
	const char *label = get_label();
	if (!label || strcmp(label, "dltest") != 0) {
		printf("dl: get_label returned '%s', want 'dltest'\n",
		       label ? label : "(null)");
		return 1;
	}

	printf("dl: dlopen/dlsym round-trip OK (handle=%p answer=%d label=%s)\n",
	       h, r, label);
	return 0;
}

static const struct test_entry tests[] = {
	{ "fork",     test_fork },
	{ "wait",     test_wait },
	{ "sig",      test_sig },
	{ "mask",     test_mask },
	{ "segv",     test_segv },
	{ "pipe",     test_pipe },
	{ "pipework", test_pipework },
	{ "malloc",   test_malloc },
	{ "udp",      test_udp },
	{ "pktfilt",  test_pktfilt },
	{ "tcp",      test_tcp },
	{ "tcpmulti", test_tcpmulti },
	{ "tcpbig",   test_tcpbig },
	{ "dl",       test_dl },
};

#define N_TESTS (sizeof(tests) / sizeof(tests[0]))

static void print_list(void)
{
	puts("usage: test <name>|all|list");
	puts("available:");
	for (unsigned i = 0; i < N_TESTS; i++) {
		printf("  %s\n", tests[i].name);
	}
}

int main(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "list") == 0) {
		print_list();
		return 0;
	}

	if (strcmp(argv[1], "all") == 0) {
		int passed = 0, failed = 0;
		for (unsigned i = 0; i < N_TESTS; i++) {
			printf("=== %s ===\n", tests[i].name);
			if (tests[i].fn() == 0) passed++; else failed++;
		}
		printf("test: %d passed, %d failed (of %u)\n",
		       passed, failed, (unsigned)N_TESTS);
		return failed ? 1 : 0;
	}

	for (unsigned i = 0; i < N_TESTS; i++) {
		if (strcmp(argv[1], tests[i].name) == 0)
			return tests[i].fn();
	}

	printf("test: unknown '%s'\n", argv[1]);
	print_list();
	return 1;
}

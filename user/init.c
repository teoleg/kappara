/*
 * user/init.c -- kappara's first userspace process: the shell
 *
 * Pid 1.  Runs entirely in EL0; every keystroke and every output
 * byte goes through a syscall.  This file used to be a 30-line "log
 * hello and yield" demo; it's now a real interactive shell, the
 * successor to the kernel-mode ksh.c that lived under kernel/.
 *
 * Boot flow
 * ---------
 *   user_spawn() (kernel/user.c) creates a kthread whose first run
 *   eret's into EL0 at USER_VA (= 0x10000000), which is _start
 *   below (forced first by user/linker.ld).  _start opens
 *   /dev/console, prints a welcome line, and falls into the
 *   read-line / parse / dispatch loop.
 *
 * I/O model
 * ---------
 *   The shell holds two file descriptors throughout its life:
 *     fd_console -- opened on /dev/console; used for input and
 *                   echoing typed characters back, and for any
 *                   command output.  The first open of /dev/console
 *                   becomes console_active in the kernel, so the
 *                   uart_rx kthread feeds bytes into our read queue.
 *     fd_user    -- a "current" fd updated by `open`; commands like
 *                   `read`, `write`, `push`, `pop`, `close` operate
 *                   on this one.
 *
 *   Echo is implemented as sys_write of one byte at a time on
 *   fd_console.  Yes, that's one SVC per typed character; for an
 *   interactive shell at human speed it's fine.
 *
 * Commands
 * --------
 *   help / pid / ls / open / close / read / write / push / pop
 *   (No `tree` -- the kernel-only vfs_dump_tree isn't exposed via
 *   syscall yet.)
 */

#include "syscall.h"

/* -------- tiny libc-substitute -------- */

static size_t ustrlen(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	return n;
}

static int ustrcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static char *udec(char *dst, long v)
{
	char tmp[24];
	int n = 0;
	if (v == 0) { tmp[n++] = '0'; }
	else {
		long x = v < 0 ? -v : v;
		while (x) { tmp[n++] = (char)('0' + (x % 10)); x /= 10; }
		if (v < 0) tmp[n++] = '-';
	}
	while (n--) *dst++ = tmp[n];
	*dst = '\0';
	return dst;
}

static long uparse_long(const char *s)
{
	long v = 0;
	int neg = 0;
	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
	return neg ? -v : v;
}

/* -------- shell state -------- */

static int fd_console = -1;
static int fd_user    = -1;

#define LINE_MAX	128
#define TOK_MAX		8

/* -------- console output helpers -------- */

static void cwriten(const char *s, size_t n)
{
	sys_write(fd_console, s, n);
}

static void cwrite(const char *s)
{
	cwriten(s, ustrlen(s));
}

static void cputc(char c)
{
	cwriten(&c, 1);
}

static void cprint_long(long v)
{
	char buf[24];
	udec(buf, v);
	cwrite(buf);
}

/* -------- line input via sys_read on fd_console -------- */

static void prompt(void)
{
	cwrite("kappara# ");
}

static int read_one(void)
{
	char c;
	long n = sys_read(fd_console, &c, 1);
	if (n != 1)
		return -1;
	return (unsigned char)c;
}

static void read_line(char *buf, size_t cap)
{
	size_t i = 0;
	for (;;) {
		int c = read_one();
		if (c < 0) { sys_yield(); continue; }
		if (c == '\r' || c == '\n') {
			cwrite("\r\n");
			break;
		}
		if (c == 0x08 || c == 0x7F) {
			if (i > 0) { i--; cwrite("\b \b"); }
			continue;
		}
		if (i + 1 < cap) {
			buf[i++] = (char)c;
			cputc((char)c);
		}
	}
	buf[i] = '\0';
}

/* -------- tokenize on spaces -------- */

static int tokenize(char *line, char *argv[], int max)
{
	int argc = 0;
	char *p = line;
	while (*p && argc < max) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t') p++;
		if (*p) { *p = '\0'; p++; }
	}
	return argc;
}

/* -------- command implementations -------- */

static void cmd_help(void)
{
	cwrite("commands:\r\n"
		"  help\r\n"
		"  pid                    sys_getpid\r\n"
		"  ls [path]              list a directory\r\n"
		"  open <path>            open and remember as $fd\r\n"
		"  close                  close $fd\r\n"
		"  read [n]               read up to n bytes from $fd\r\n"
		"  write <text...>        write text to $fd\r\n"
		"  push <module>          ioctl(I_PUSH) on $fd\r\n"
		"  pop                    ioctl(I_POP) on $fd\r\n");
}

static void cmd_pid(void)
{
	cprint_long(sys_getpid());
	cwrite("\r\n");
}

static void cmd_ls(int argc, char *argv[])
{
	const char *path = (argc > 1) ? argv[1] : "/";
	char out[256];
	long n = sys_ls(path, out, sizeof(out));
	if (n < 0) {
		cwrite("ls: cannot access '"); cwrite(path); cwrite("'\r\n");
		return;
	}
	for (long i = 0; i < n; i++) {
		if (out[i] == '\n') cputc('\r');
		cputc(out[i]);
	}
}

static void cmd_open(int argc, char *argv[])
{
	if (argc < 2) { cwrite("usage: open <path>\r\n"); return; }
	if (fd_user >= 0) { sys_close(fd_user); fd_user = -1; }
	long fd = sys_open(argv[1]);
	if (fd < 0) { cwrite("open: failed\r\n"); return; }
	fd_user = (int)fd;
	cwrite("fd="); cprint_long(fd); cwrite("\r\n");
}

static void cmd_close(void)
{
	if (fd_user < 0) { cwrite("no open fd\r\n"); return; }
	sys_close(fd_user);
	fd_user = -1;
}

static void cmd_read(int argc, char *argv[])
{
	if (fd_user < 0) { cwrite("no open fd\r\n"); return; }
	long want = (argc > 1) ? uparse_long(argv[1]) : 128;
	if (want <= 0 || want > 256) want = 128;
	char buf[256];
	long n = sys_read(fd_user, buf, (size_t)want);
	if (n <= 0) {
		cwrite("read: ");
		cprint_long(n);
		cwrite("\r\n");
		return;
	}
	for (long i = 0; i < n; i++) {
		if (buf[i] == '\n') cputc('\r');
		cputc(buf[i]);
	}
	if (n > 0 && buf[n - 1] != '\n')
		cwrite("\r\n");
}

static void cmd_write(int argc, char *argv[])
{
	if (fd_user < 0) { cwrite("no open fd\r\n"); return; }
	if (argc < 2) { cwrite("usage: write <text>\r\n"); return; }
	char buf[256];
	size_t off = 0;
	for (int i = 1; i < argc; i++) {
		if (i > 1 && off + 1 < sizeof(buf))
			buf[off++] = ' ';
		size_t l = ustrlen(argv[i]);
		if (off + l > sizeof(buf)) l = sizeof(buf) - off;
		for (size_t j = 0; j < l; j++) buf[off + j] = argv[i][j];
		off += l;
	}
	long w = sys_write(fd_user, buf, off);
	cprint_long(w); cwrite(" bytes\r\n");
}

static void cmd_push(int argc, char *argv[])
{
	if (fd_user < 0) { cwrite("no open fd\r\n"); return; }
	if (argc < 2) { cwrite("usage: push <module>\r\n"); return; }
	long r = sys_ioctl(fd_user, I_PUSH, (long)(unsigned long)argv[1]);
	cwrite("push: "); cprint_long(r); cwrite("\r\n");
}

static void cmd_pop(void)
{
	if (fd_user < 0) { cwrite("no open fd\r\n"); return; }
	long r = sys_ioctl(fd_user, I_POP, 0);
	cwrite("pop: "); cprint_long(r); cwrite("\r\n");
}

/* -------- dispatch -------- */

static void dispatch(char *line)
{
	char *argv[TOK_MAX];
	int argc = tokenize(line, argv, TOK_MAX);
	if (argc == 0) return;

	if      (!ustrcmp(argv[0], "help"))  cmd_help();
	else if (!ustrcmp(argv[0], "pid"))   cmd_pid();
	else if (!ustrcmp(argv[0], "ls"))    cmd_ls(argc, argv);
	else if (!ustrcmp(argv[0], "open"))  cmd_open(argc, argv);
	else if (!ustrcmp(argv[0], "close")) cmd_close();
	else if (!ustrcmp(argv[0], "read"))  cmd_read(argc, argv);
	else if (!ustrcmp(argv[0], "write")) cmd_write(argc, argv);
	else if (!ustrcmp(argv[0], "push"))  cmd_push(argc, argv);
	else if (!ustrcmp(argv[0], "pop"))   cmd_pop();
	else {
		cwrite(argv[0]); cwrite(": command not found\r\n");
	}
}

/* -------- entry -------- */

__attribute__((noreturn, section(".text._start")))
void _start(void)
{
	{
		char buf[80];
		const char *p = "init: pid=";
		char *q = buf;
		while (*p) *q++ = *p++;
		udec(q, sys_getpid());
		sys_log(buf);
	}

	fd_console = (int)sys_open("/dev/console");
	if (fd_console < 0) {
		sys_log("init: open /dev/console failed");
		for (;;) sys_yield();
	}

	cwrite("\r\nkappara shell (userspace) -- type 'help' for commands\r\n");

	char line[LINE_MAX];
	for (;;) {
		prompt();
		read_line(line, sizeof(line));
		dispatch(line);
	}
}

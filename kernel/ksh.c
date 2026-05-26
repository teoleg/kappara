/*
 * kernel/ksh.c -- kappara shell ("ksh")
 * =====================================
 *
 * The simplest possible shell that actually drives the kernel from
 * the keyboard.  Runs as a kthread (started from kmain), reads one
 * line at a time from the PL011 UART, parses it into space-separated
 * tokens, dispatches the verb to a syscall.
 *
 * Why this exists
 * ---------------
 * Without a user interface, the filesystem and syscalls are invisible.
 * ksh is the user interface.  It is intentionally a kernel thread
 * (not a userspace process) because we don't have EL0/USR yet -- the
 * commands run via the same SVC #0 path that any future userspace
 * shell would use, so the syscall layer is exercised exactly the
 * same way.
 *
 * Input
 * -----
 * uart_getc_nonblock polls the PL011 RX FIFO.  In QEMU with
 * "-serial stdio" the host terminal pipes keystrokes into that FIFO,
 * so typing in the terminal works directly.  Between polls ksh
 * calls kthread_yield, so the other kthreads (and the timer-driven
 * preemption) all keep ticking; ksh doesn't hog the CPU while
 * waiting for you to press a key.
 *
 * Line editing
 * ------------
 * Backspace (0x08) and DEL (0x7F) both erase one character.  Enter
 * (\r or \n) submits.  Everything else is appended, echoed, and shown.
 *
 * Commands
 * --------
 *   help                          show this list
 *   ls [path]                     list directory contents
 *   tree                          dump the whole filesystem tree
 *   pid                           print current thread's tid
 *   open <path>                   open and remember as $fd (one slot)
 *   close                         close $fd
 *   write <text...>               write text to $fd
 *   read [n]                      read up to n bytes from $fd
 *   push <module>                 ioctl(I_PUSH, module) on $fd
 *   pop                           ioctl(I_POP) on $fd
 *
 * "$fd" is shell state -- a single saved fd that open updates and
 * close/read/write/push/pop refer to.  A real shell would have many.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/ksh.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/stream_head.h"
#include "kappara/string.h"
#include "kappara/syscall.h"
#include "kappara/uart.h"
#include "kappara/vfs.h"

#define LINE_MAX	128
#define TOK_MAX		8

static long ksh_fd       = -1;	/* user-current fd                */
static long ksh_stdin_fd = -1;	/* /dev/console; one byte at a time */

/* ---- Local syscall helpers (AArch64 + ARMv7 inline asm) ---------------- */

#if defined(__aarch64__)

static long do_sc(long num, long a0, long a1, long a2)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x8 __asm__("x8") = num;
	__asm__ volatile (
		"svc	#0\n"
		: "+r"(x0)
		: "r"(x1), "r"(x2), "r"(x8)
		: "memory", "cc");
	return x0;
}

#else  /* ARMv7 EABI */

static long do_sc(long num, long a0, long a1, long a2)
{
	register long r0 __asm__("r0") = a0;
	register long r1 __asm__("r1") = a1;
	register long r2 __asm__("r2") = a2;
	register long r7 __asm__("r7") = num;
	__asm__ volatile (
		"svc	#0\n"
		: "+r"(r0)
		: "r"(r1), "r"(r2), "r"(r7)
		: "memory", "cc");
	return r0;
}

#endif

/* ---- Line input -------------------------------------------------------- */

static void prompt(void)
{
	uart_puts("kappara# ");
}

/* Pull one character from /dev/console via SYS_read.  Returns -1
 * on no-data-yet so the caller can yield and try again. */
static int read_one_char(void)
{
	char c;
	long n = do_sc(SYS_read, ksh_stdin_fd, (long)(uintptr_t)&c, 1);
	if (n != 1)
		return -1;
	return (unsigned char)c;
}

static void read_line(char *buf, size_t cap)
{
	size_t i = 0;
	for (;;) {
		int c = read_one_char();
		if (c < 0) {
			do_sc(SYS_yield, 0, 0, 0);
			continue;
		}
		if (c == '\r' || c == '\n') {
			uart_putc('\r');
			uart_putc('\n');
			break;
		}
		if (c == 0x08 || c == 0x7F) {
			if (i > 0) {
				i--;
				uart_putc('\b');
				uart_putc(' ');
				uart_putc('\b');
			}
			continue;
		}
		if (i + 1 < cap) {
			buf[i++] = (char)c;
			uart_putc((char)c);
		}
	}
	buf[i] = '\0';
}

/* ---- Tokenise on spaces ----------------------------------------------- */

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

static long parse_long(const char *s)
{
	long v = 0;
	int neg = 0;
	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (*s - '0');
		s++;
	}
	return neg ? -v : v;
}

/* ---- Command implementations ----------------------------------------- */

static void cmd_help(void)
{
	uart_puts("commands:\n"
		"  help\n"
		"  ls [path]              list directory\n"
		"  tree                   dump filesystem tree\n"
		"  pid                    print current tid\n"
		"  open <path>            open and remember as $fd\n"
		"  close                  close $fd\n"
		"  read [n]               read from $fd (default n=128)\n"
		"  write <text...>        write text to $fd\n"
		"  push <module>          ioctl(I_PUSH, module) on $fd\n"
		"  pop                    ioctl(I_POP) on $fd\n");
}

static void cmd_ls(int argc, char *argv[])
{
	const char *path = (argc > 1) ? argv[1] : "/";
	char out[256];
	long n = do_sc(SYS_ls, (long)(uintptr_t)path, (long)(uintptr_t)out,
		       sizeof(out));
	if (n < 0) {
		kprintf("ls: cannot access '%s'\n", path);
		return;
	}
	for (long i = 0; i < n; i++)
		uart_putc(out[i]);
}

static void cmd_open(int argc, char *argv[])
{
	if (argc < 2) { uart_puts("usage: open <path>\n"); return; }
	if (ksh_fd >= 0) {
		do_sc(SYS_close, ksh_fd, 0, 0);
		ksh_fd = -1;
	}
	long fd = do_sc(SYS_open, (long)(uintptr_t)argv[1], 0, 0);
	if (fd < 0) {
		kprintf("open: '%s' failed\n", argv[1]);
		return;
	}
	ksh_fd = fd;
	kprintf("fd=%ld\n", fd);
}

static void cmd_close(void)
{
	if (ksh_fd < 0) { uart_puts("no open fd\n"); return; }
	do_sc(SYS_close, ksh_fd, 0, 0);
	ksh_fd = -1;
}

static void cmd_read(int argc, char *argv[])
{
	if (ksh_fd < 0) { uart_puts("no open fd\n"); return; }
	long want = (argc > 1) ? parse_long(argv[1]) : 128;
	if (want <= 0 || want > 256) want = 128;
	char buf[256];
	long n = do_sc(SYS_read, ksh_fd, (long)(uintptr_t)buf, want);
	if (n <= 0) { kprintf("read: %ld\n", n); return; }
	for (long i = 0; i < n; i++)
		uart_putc(buf[i]);
	if (n > 0 && buf[n - 1] != '\n')
		uart_putc('\n');
}

static void cmd_write(int argc, char *argv[])
{
	if (ksh_fd < 0) { uart_puts("no open fd\n"); return; }
	if (argc < 2) { uart_puts("usage: write <text>\n"); return; }
	/* Join argv[1..argc-1] with single spaces.  We deliberately don't
	 * try to recover the original whitespace -- tokenize() already
	 * collapsed runs of spaces, and that's fine for a demo shell. */
	char buf[256];
	size_t off = 0;
	for (int i = 1; i < argc; i++) {
		if (i > 1 && off + 1 < sizeof(buf))
			buf[off++] = ' ';
		size_t l = kstrlen(argv[i]);
		if (off + l > sizeof(buf))
			l = sizeof(buf) - off;
		for (size_t j = 0; j < l; j++)
			buf[off + j] = argv[i][j];
		off += l;
	}
	long w = do_sc(SYS_write, ksh_fd, (long)(uintptr_t)buf, (long)off);
	kprintf("%ld bytes\n", w);
}

static void cmd_push(int argc, char *argv[])
{
	if (ksh_fd < 0) { uart_puts("no open fd\n"); return; }
	if (argc < 2) { uart_puts("usage: push <module>\n"); return; }
	long r = do_sc(SYS_ioctl, ksh_fd, I_PUSH, (long)(uintptr_t)argv[1]);
	kprintf("push: %ld\n", r);
}

static void cmd_pop(void)
{
	if (ksh_fd < 0) { uart_puts("no open fd\n"); return; }
	long r = do_sc(SYS_ioctl, ksh_fd, I_POP, 0);
	kprintf("pop: %ld\n", r);
}

/* ---- Dispatch -------------------------------------------------------- */

static void dispatch(char *line)
{
	char *argv[TOK_MAX];
	int argc = tokenize(line, argv, TOK_MAX);
	if (argc == 0)
		return;

	if      (kstrcmp(argv[0], "help")  == 0) cmd_help();
	else if (kstrcmp(argv[0], "ls")    == 0) cmd_ls(argc, argv);
	else if (kstrcmp(argv[0], "tree")  == 0) vfs_dump_tree(vfs_root());
	else if (kstrcmp(argv[0], "pid")   == 0) {
		long p = do_sc(SYS_getpid, 0, 0, 0);
		kprintf("%ld\n", p);
	}
	else if (kstrcmp(argv[0], "open")  == 0) cmd_open(argc, argv);
	else if (kstrcmp(argv[0], "close") == 0) cmd_close();
	else if (kstrcmp(argv[0], "read")  == 0) cmd_read(argc, argv);
	else if (kstrcmp(argv[0], "write") == 0) cmd_write(argc, argv);
	else if (kstrcmp(argv[0], "push")  == 0) cmd_push(argc, argv);
	else if (kstrcmp(argv[0], "pop")   == 0) cmd_pop();
	else
		kprintf("%s: command not found\n", argv[0]);
}

/* ---- Entry point ------------------------------------------------------ */

void ksh_main(void *arg)
{
	(void)arg;

	/* Take ownership of console RX by opening /dev/console first.
	 * stream_build registers ksh's stdata as console_active, which
	 * is what uart_rx_main targets when it pushes received bytes. */
	ksh_stdin_fd = do_sc(SYS_open, (long)(uintptr_t)"/dev/console", 0, 0);
	if (ksh_stdin_fd < 0) {
		uart_puts("ksh: cannot open /dev/console\n");
		return;
	}

	uart_puts("\n");
	uart_puts("kappara shell -- type 'help' for commands\n");
	char line[LINE_MAX];
	for (;;) {
		prompt();
		read_line(line, sizeof(line));
		dispatch(line);
	}
}

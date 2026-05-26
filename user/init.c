/*
 * user/init.c -- the first AArch64 EL0 program in kappara
 *
 * Cross-compiled by the top-level Makefile, linked at virtual
 * address 0x10000000 (matches USER_VA in kernel/user.c), objcopy'd
 * to a raw binary, embedded into the kernel via .incbin, and copied
 * into the 2 MB user backing region at boot.  When user_spawn()
 * eret's into EL0, control lands at _start below.
 *
 * Everything below talks to the kernel through SVC #0; there is no
 * libc, no startup framework.  Function entry is _start.
 *
 * What this user program demonstrates
 * -----------------------------------
 *   1. sys_log + sys_getpid: a simple syscall round trip with a
 *      pointer the kernel has to read from user memory.
 *   2. sys_open / sys_write / sys_close on /dev/console: bytes
 *      written from EL0 traverse the full STREAMS path (head_wq
 *      -> console_wq_putp -> uart_putc) before hitting the serial
 *      port.
 *   3. sys_open / sys_read on /dev/klog: pull the boot log back
 *      and re-emit it through /dev/console -- a non-trivial
 *      user-side pipeline routing two streams together.
 *
 * After that it loops on sys_yield forever, so the user thread
 * stays alive and ksh runs alongside.
 */

#include "syscall.h"

/* --- tiny libc-substitute helpers ------------------------------------ */

static size_t ustrlen(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	return n;
}

static char *ucat(char *dst, const char *src)
{
	while (*src) *dst++ = *src++;
	return dst;
}

static char *udec(char *dst, long v)
{
	char tmp[24];
	int n = 0;
	if (v == 0) {
		tmp[n++] = '0';
	} else {
		long x = v < 0 ? -v : v;
		while (x) { tmp[n++] = (char)('0' + (x % 10)); x /= 10; }
		if (v < 0) tmp[n++] = '-';
	}
	while (n--) *dst++ = tmp[n];
	return dst;
}

/* --- the program ----------------------------------------------------- */

static int fd_console;

static void cwrite(const char *s)
{
	sys_write(fd_console, s, ustrlen(s));
}

static void cwriten(const char *s, size_t n)
{
	sys_write(fd_console, s, n);
}

__attribute__((noreturn, section(".text._start")))
void _start(void)
{
	long pid = sys_getpid();

	/* Greeting via sys_log (raw kernel kprintf). */
	{
		char buf[80];
		char *p = ucat(buf, "init: hello from real user binary (pid=");
		p = udec(p, pid);
		p = ucat(p, ")");
		*p = '\0';
		sys_log(buf);
	}

	/* Open /dev/console and write a line through STREAMS. */
	fd_console = (int)sys_open("/dev/console");
	if (fd_console < 0) {
		sys_log("init: open /dev/console failed");
		for (;;) sys_yield();
	}
	cwrite("init: writing this line through STREAMS from EL0\r\n");

	/* Dump the boot log: open /dev/klog and copy each chunk to console. */
	cwrite("init: --- boot log replay (via /dev/klog) ---\r\n");
	int klog = (int)sys_open("/dev/klog");
	if (klog >= 0) {
		char buf[256];
		long n;
		for (;;) {
			n = sys_read(klog, buf, sizeof(buf));
			if (n <= 0) break;
			cwriten(buf, (size_t)n);
		}
		sys_close(klog);
	}
	cwrite("init: --- end of boot log ---\r\n");

	sys_close(fd_console);

	sys_log("init: done, looping forever");
	for (;;) sys_yield();
}

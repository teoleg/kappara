/*
 * user/init.c -- the first AArch64 EL0 program in kappara
 *
 * Compiled separately by the top-level Makefile, linked at virtual
 * address 0x10000000 (matches USER_VA in kernel/user.c), objcopy'd
 * to a raw binary, embedded into the kernel via .incbin, and copied
 * into the 2 MB user backing region at boot.  When user_spawn()
 * eret's into EL0, control lands at _start below.
 *
 * What it does
 * ------------
 *   1. logs "init: hello from real user binary (pid=N)"
 *   2. loops yielding the CPU N_ITER times
 *   3. logs "init: done, looping forever"
 *   4. spins on sys_yield() forever (so user mode stays alive and
 *      ksh sees the syscalls keep coming in)
 *
 * Everything goes through SVC; this binary is link-time linked
 * against nothing except itself.
 */

#include "syscall.h"

/* Tiny manual itoa so we can show changing values without sprintf. */
static char *render_pid(char *buf, long pid)
{
	char tmp[24];
	int n = 0;
	if (pid == 0) {
		tmp[n++] = '0';
	} else {
		long v = pid;
		while (v) {
			tmp[n++] = (char)('0' + (v % 10));
			v /= 10;
		}
	}
	char *p = buf;
	while (n--)
		*p++ = tmp[n];
	*p = '\0';
	return buf;
}

__attribute__((noreturn, section(".text._start")))
void _start(void)
{
	long pid = sys_getpid();

	char msg[64];
	const char *prefix = "init: hello from real user binary (pid=";
	char *p = msg;
	while (*prefix)
		*p++ = *prefix++;
	render_pid(p, pid);
	while (*p)
		p++;
	*p++ = ')';
	*p = '\0';
	sys_log(msg);

	for (int i = 0; i < 5; i++)
		sys_yield();

	sys_log("init: done, looping forever");

	for (;;)
		sys_yield();
}

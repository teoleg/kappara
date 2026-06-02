/*
 * user/hello.c -- simplest ELF user program, for testing sys_execve
 *
 * Compiled as a standalone AArch64 ELF at VA 0x20000000 (prog_linker.ld).
 * Writes a line to fd 1 (inherited from the shell) then exits.
 */
#include "syscall.h"

static const char msg[] = "Hello from exec'd ELF!\r\n";

void _start(void)
{
	/* fd 1 = /dev/console, inherited via kthread_inherit_fds */
	sys_write(1, msg, sizeof(msg) - 1);

	/* sys_exit -- no libc atexit, no return */
	register long x8 __asm__("x8") = SYS_exit;
	__asm__ volatile ("svc #0" :: "r"(x8) : "memory");
	for (;;)
		;
}

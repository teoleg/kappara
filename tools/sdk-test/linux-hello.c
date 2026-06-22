/*
 * tools/sdk-test/linux-hello.c -- INDIE.md Path B stage 1 smoke binary.
 *
 * Pretends to be a vanilla Linux aarch64 static-pie program: uses the
 * Linux syscall numbers (write=64, exit=93) via raw svc, no libc.  If
 * this prints "hello from Linux-ABI ..." it proves kappara translates
 * Linux syscalls into its native handlers.
 *
 * Built externally with `aarch64-linux-gnu-gcc -static-pie -nostdlib`,
 * no kappara headers in sight.
 */

static long linux_write(int fd, const void *buf, long n)
{
	register long x0 __asm__("x0") = fd;
	register long x1 __asm__("x1") = (long)buf;
	register long x2 __asm__("x2") = n;
	register long x8 __asm__("x8") = 64;	/* Linux SYS_write */
	__asm__ volatile ("svc #0"
			  : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
	return x0;
}

static void linux_exit(int code) __attribute__((noreturn));
static void linux_exit(int code)
{
	register long x0 __asm__("x0") = code;
	register long x8 __asm__("x8") = 93;	/* Linux SYS_exit */
	__asm__ volatile ("svc #0" :: "r"(x0), "r"(x8));
	__builtin_unreachable();
}

void _start(void)
{
	static const char msg[] =
		"hello from Linux-ABI static-pie on kappara\n";
	linux_write(1, msg, sizeof(msg) - 1);
	linux_exit(0);
}

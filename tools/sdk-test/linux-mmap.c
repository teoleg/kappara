/*
 * tools/sdk-test/linux-mmap.c -- INDIE.md Path B stage 3 smoke binary.
 *
 * Calls Linux SYS_mmap to allocate a page, writes a magic string into
 * it, prints the string back via SYS_write, then SYS_munmap's and
 * exits.  Proves the anonymous mmap path round-trips end-to-end.
 */

static long lsys(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x5 __asm__("x5") = a5;
	register long x8 __asm__("x8") = nr;
	__asm__ volatile ("svc #0"
			  : "+r"(x0)
			  : "r"(x1), "r"(x2), "r"(x3),
			    "r"(x4), "r"(x5), "r"(x8)
			  : "memory");
	return x0;
}

#define SYS_write    64
#define SYS_exit     93
#define SYS_mmap    222
#define SYS_munmap  215

#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define MAP_PRIVATE  0x2
#define MAP_ANON     0x20

void _start(void)
{
	static const char ok_msg[]   = "mmap-test: page contents OK\n";
	static const char fail_msg[] = "mmap-test: FAIL\n";

	long page = lsys(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
	                 MAP_PRIVATE | MAP_ANON, -1, 0);
	if (page < 0 || page == 0) {
		lsys(SYS_write, 1, (long)fail_msg, sizeof(fail_msg) - 1, 0, 0, 0);
		lsys(SYS_exit,  1, 0, 0, 0, 0, 0);
	}

	/* Write a marker, read back, verify. */
	char *p = (char *)(unsigned long)page;
	const char magic[] = "kappara mmap roundtrip\n";
	int i = 0;
	for (; magic[i]; i++) p[i] = magic[i];
	p[i] = '\0';

	int match = 1;
	for (int j = 0; magic[j]; j++) if (p[j] != magic[j]) { match = 0; break; }

	if (match) {
		lsys(SYS_write, 1, (long)ok_msg, sizeof(ok_msg) - 1, 0, 0, 0);
		lsys(SYS_write, 1, (long)p,      (long)i,            0, 0, 0);
	} else {
		lsys(SYS_write, 1, (long)fail_msg, sizeof(fail_msg) - 1, 0, 0, 0);
	}

	lsys(SYS_munmap, page, 4096, 0, 0, 0, 0);
	lsys(SYS_exit,   0, 0, 0, 0, 0, 0);
	__builtin_unreachable();
}

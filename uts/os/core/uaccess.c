/*
 * kernel/uaccess.c -- bounds-checked copy between user and kernel
 * ===============================================================
 *
 * See include/kappara/core/uaccess.h for the contract.  This file is
 * the implementation: a per-syscall flag (syscall_from_user) plus
 * three byte-copy primitives, all of which refuse pointers outside
 * the user-mapped window.
 *
 * Trust model
 * -----------
 * "User window" = USER_VA .. USER_VA + USER_SIZE.  Anything in there
 * was either copied in from the user binary at boot or written by
 * user code via syscalls -- the kernel doesn't trust the contents
 * (a malicious user could put anything there), but it does trust
 * that the address is harmless to dereference (the page is mapped
 * Normal cacheable, not Device, no MMIO side-effects).
 *
 * Anything OUTSIDE the user window is one of:
 *   - kernel text/data (would leak kernel memory if read)
 *   - MMIO peripherals at 0x3F000000 (would do real I/O side-effects)
 *   - unmapped (would fault)
 * All three are bugs and the kernel refuses.
 *
 * What's not yet checked
 * ----------------------
 * We trust syscall_from_user to be set correctly by the arch trap
 * dispatcher.  If it's wrong (left set after a kernel-mode call,
 * or unset for a user-mode call) the trust model breaks.  Setting
 * + clearing it bracket the syscall_dispatch call in arch trap.c.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/core/printk.h"
#include "kappara/core/uaccess.h"

/*
 * Recognised user windows.  Keep in sync with kernel/user.c.
 *
 *   init window  0x10000000 .. 0x10200000  (2 MB: code + spawn stacks)
 *   exec window  0x20000000 .. 0x20400000  (4 MB: 2 MB code + 2 MB stack)
 *
 * When we grow to per-process page tables this becomes a per-task
 * range check; for now two static windows is correct.
 */
#define USER_VA		0x10000000UL
/* DYNAMIC.md + INDIE.md windows -- keep in sync with
 * uts/os/user/user.c.  EXEC_TOTAL covers code+stack+heap as one
 * contiguous span. */
#define EXEC_VA		0x20000000UL
#define EXEC_TOTAL	0x01600000UL	/* 16 MB code + 2 MB stack + 4 MB heap */
#define ANON_VA		0x22000000UL
#define ANON_SIZE	0x08000000UL
#define LD_VA		0x30000000UL
#define LD_SIZE		0x00100000UL
#define LIBC_VA		0x38000000UL
#define LIBC_SIZE	0x00200000UL
#define DLOPEN_VA	0x39000000UL
#define DLOPEN_SIZE	0x00200000UL
#define USER_SIZE	0x00200000UL


int syscall_from_user;

int user_ptr_ok(const void *p, size_t n)
{
	uintptr_t a = (uintptr_t)p;
	uintptr_t end;
	if (__builtin_add_overflow(a, n, &end))
		return 0;
	if (n == 0)
		end = a + 1;	/* treat zero-length as a 1-byte probe */

	if (a >= USER_VA && end <= USER_VA + USER_SIZE)
		return 1;
	if (a >= EXEC_VA && end <= EXEC_VA + EXEC_TOTAL)
		return 1;
	/* INDIE.md Path B stage 3: anonymous mmap range. */
	if (a >= ANON_VA && end <= ANON_VA + ANON_SIZE)
		return 1;
	/* DYNAMIC.md stages 5-7: dynamic linker, libc.so, dlopen slots. */
	if (a >= LD_VA && end <= LD_VA + LD_SIZE)
		return 1;
	if (a >= LIBC_VA && end <= LIBC_VA + LIBC_SIZE)
		return 1;
	if (a >= DLOPEN_VA && end <= DLOPEN_VA + DLOPEN_SIZE)
		return 1;
	return 0;
}

long copy_from_user(void *dst, const void *usrc, size_t n)
{
	if (!user_ptr_ok(usrc, n))
		return -1;
	const unsigned char *s = usrc;
	unsigned char *d = dst;
	for (size_t i = 0; i < n; i++)
		d[i] = s[i];
	return 0;
}

long copy_to_user(void *udst, const void *src, size_t n)
{
	if (!user_ptr_ok(udst, n))
		return -1;
	const unsigned char *s = src;
	unsigned char *d = udst;
	for (size_t i = 0; i < n; i++)
		d[i] = s[i];
	return 0;
}

long strncpy_from_user(char *dst, const char *usrc, size_t maxlen)
{
	if (maxlen == 0)
		return -1;
	for (size_t i = 0; i < maxlen; i++) {
		if (!user_ptr_ok(usrc + i, 1))
			return -1;
		char c = usrc[i];
		dst[i] = c;
		if (c == '\0')
			return (long)i;
	}
	/* No NUL within maxlen; refuse rather than truncate-without-NUL. */
	return -1;
}

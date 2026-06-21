/*
 * lib/libdltest/dltest.c -- shared object for testing dlopen/dlsym
 *
 * Has DT_NEEDED libc.so so its calls to printf / strlen go through the
 * cross-DSO resolver -- exercising the same JUMP_SLOT / GLOB_DAT path
 * that execve uses for cmd binaries.  cmd/test.c's "dl" subtest dlopens
 * this, dlsyms three functions, and checks their return values.
 */

#include <stdio.h>
#include <string.h>

static const char *dltest_label = "dltest";

int dltest_answer(void)
{
	return 42;
}

const char *dltest_get_label(void)
{
	/* Hitting `dltest_label` here forces a GOT entry that needs
	 * R_AARCH64_RELATIVE -- a one-instruction self-resolving smoke
	 * test for the dlopen reloc walk. */
	return dltest_label;
}

/* Exercises cross-DSO calls back into libc.so.  Returns the length
 * of its own label string as reported by libc's strlen, plus prints
 * a message to stdout via libc's printf.  Both calls go through
 * JUMP_SLOT entries the dlopen resolver writes against libc.so. */
int dltest_use_libc(void)
{
	printf("dltest: hello from a dlopen'd .so via libc.so printf\n");
	return (int)strlen(dltest_label);
}

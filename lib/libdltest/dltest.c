/*
 * lib/libdltest/dltest.c -- minimal shared object for testing dlopen
 *
 * Self-contained: no DT_NEEDED, no libc references.  Just two exported
 * functions and one absolute-pointer global to prove R_AARCH64_RELATIVE
 * applies correctly on dlopen.
 *
 * cmd/test.c's "dl" subtest dlopens this, dlsym's "dltest_answer", and
 * checks the return value is 42.
 */

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

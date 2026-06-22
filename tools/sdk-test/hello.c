/*
 * tools/sdk-test/hello.c -- INDIE.md stage 1 smoke test.
 *
 * This file pretends to be an external project: it includes only
 * <stdio.h> + <stdlib.h> from the SDK's sysroot, doesn't touch any
 * uts/ headers, and is built with kappara-cc (not the in-tree Makefile).
 * If it prints "hello from outside the tree", the SDK works.
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	printf("hello from outside the tree\n");
	printf("argc=%d argv[0]=%s\n", argc, argv[0]);
	if (argc > 1)
		printf("first arg: %s (atoi: %d)\n", argv[1], atoi(argv[1]));
	return 0;
}

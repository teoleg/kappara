/*
 * cmd/rm.c -- standalone `rm`.  Takes one or more paths, unlinks
 * each.  No flags yet; no `-r` because we'd need a recursive
 * dirent walker.
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		puts("usage: rm <path> [...]");
		return 1;
	}
	int err = 0;
	for (int i = 1; i < argc; i++) {
		if (unlink(argv[i]) < 0) {
			printf("rm: cannot remove '%s'\n", argv[i]);
			err = 1;
		}
	}
	return err;
}

/*
 * cmd/cat.c -- standalone `cat`.  Opens each named file, reads it
 * in 1 KB chunks, writes to stdout.  Returns 1 if any file failed
 * to open so the shell sees a non-zero status; remaining files
 * still get printed (so `cat a missing b` prints a + b).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		puts("usage: cat [-] <path> [...]");
		return 1;
	}
	/* A leading "-" strips the plain-language "# " preamble that
	 * /proc files print (bare data for scripts / experts).  It is
	 * harmless on ordinary files, which have no such header. */
	int strip = 0, first = 1;
	if (argv[1][0] == '-' && argv[1][1] == '\0') { strip = 1; first = 2; }
	int err = 0;
	for (int i = first; i < argc; i++) {
		if (proc_cat(argv[i], strip) < 0) {
			printf("cat: cannot open '%s'\n", argv[i]);
			err = 1;
		}
	}
	return err;
}

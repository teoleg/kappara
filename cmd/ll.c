/*
 * cmd/ll.c -- "ls -l", standalone.  SYS_ll fills the supplied
 * buffer with one row per entry:  "<type> <size> <name>\n".  For
 * character devices the size column carries "major,minor".
 * Pass-through to stdout.
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *path = (argc >= 2) ? argv[1] : ".";

	char buf[2048];
	long n = ll(path, buf, sizeof(buf));
	if (n < 0) {
		printf("ll: cannot access '%s'\n", path);
		return 1;
	}
	write(1, buf, (size_t)n);
	if (n > 0 && buf[n - 1] != '\n')
		write(1, "\n", 1);
	return 0;
}

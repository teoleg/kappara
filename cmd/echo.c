/*
 * cmd/echo.c -- POSIX-shape echo.  Prints argv[1..] separated by
 * spaces then a newline.  The shell builtin `echo <path> <text...>`
 * has different semantics (writes the text into a file), so the
 * standalone version doesn't shadow it -- shell builtin still wins.
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (i > 1) write(1, " ", 1);
		size_t n = 0;
		while (argv[i][n]) n++;
		write(1, argv[i], n);
	}
	write(1, "\n", 1);
	return 0;
}

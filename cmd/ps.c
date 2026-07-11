/* cmd/ps.c -- list running threads (reads /proc/ps).
 * `ps -` prints without the plain-language preamble. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int strip = (argc > 1 && strcmp(argv[1], "-") == 0);
	if (proc_cat("/proc/ps", strip) < 0) {
		puts("ps: cannot open /proc/ps");
		return 1;
	}
	return 0;
}

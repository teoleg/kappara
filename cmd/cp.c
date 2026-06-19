/*
 * cmd/cp.c -- `cp <src> <dst>`.
 *
 * Streams bytes from src to dst.  If dst already exists, the kfs
 * regfile_write path overwrites in-place; if it doesn't, we creat
 * it first.  No `-r` for directories yet -- copying a tree wants
 * a real opendir / stat surface we don't have.
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 3) {
		puts("usage: cp <src> <dst>");
		return 1;
	}
	int src = open(argv[1], 0);
	if (src < 0) { printf("cp: cannot open src '%s'\n", argv[1]); return 1; }

	/* sys_open without O_CREAT fails on a fresh dst; ignore creat()
	 * errors because it might already exist (in which case the open
	 * below succeeds anyway).  Failure is reported at open time. */
	(void)creat(argv[2]);
	int dst = open(argv[2], 0);
	if (dst < 0) {
		printf("cp: cannot open dst '%s'\n", argv[2]);
		close(src);
		return 1;
	}

	char buf[1024];
	long n;
	long total = 0;
	while ((n = read(src, buf, sizeof(buf))) > 0) {
		long w = write(dst, buf, (size_t)n);
		if (w != n) { printf("cp: short write\n"); break; }
		total += n;
	}

	close(src);
	close(dst);
	printf("cp: %ld bytes\n", total);
	return 0;
}

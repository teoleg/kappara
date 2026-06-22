/*
 * cmd/cat.c -- standalone `cat`.  Opens each named file, reads it
 * in 1 KB chunks, writes to stdout.  Returns 1 if any file failed
 * to open so the shell sees a non-zero status; remaining files
 * still get printed (so `cat a missing b` prints a + b).
 */

#include <stdio.h>
#include <unistd.h>

static int dump(const char *path)
{
	int fd = open(path, 0);
	if (fd < 0) {
		printf("cat: cannot open '%s'\n", path);
		return -1;
	}
	char buf[1024];
	long n;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		write(1, buf, (size_t)n);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		puts("usage: cat <path> [...]");
		return 1;
	}
	int err = 0;
	for (int i = 1; i < argc; i++)
		if (dump(argv[i]) < 0) err = 1;
	return err;
}

/*
 * cmd/head.c -- print the first N lines of each named file.
 * `head <file>` defaults to 10 lines; `head -n N <file>` overrides
 * the count.  No special handling for stdin yet -- we only have
 * regular files in /usr/bin and /home.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int parse_int(const char *s)
{
	int v = 0;
	while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
	return v;
}

static int dump_head(const char *path, int n)
{
	int fd = open(path, 0);
	if (fd < 0) { printf("head: cannot open '%s'\n", path); return -1; }
	char  buf[1024];
	long  r;
	int   lines = 0;
	while (lines < n && (r = read(fd, buf, sizeof(buf))) > 0) {
		long out = 0;
		for (long i = 0; i < r && lines < n; i++) {
			if (buf[i] == '\n') lines++;
			out = i + 1;
		}
		write(1, buf, (size_t)out);
		if (lines >= n) break;
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	int n = 10;
	int i = 1;
	if (i < argc && !strcmp(argv[i], "-n") && i + 1 < argc) {
		n = parse_int(argv[i + 1]);
		i += 2;
	}
	if (i >= argc) { puts("usage: head [-n N] <file> [...]"); return 1; }

	int err = 0;
	for (; i < argc; i++)
		if (dump_head(argv[i], n) < 0) err = 1;
	return err;
}

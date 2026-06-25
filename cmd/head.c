/*
 * cmd/head.c -- print the first N lines of each named file.
 * `head <file>` defaults to 10 lines; `head -n N <file>` overrides
 * the count.  With no file argument (or `-`), reads stdin -- so
 * `cat /dev/ksyms | head` works the way the shell pipe expects.
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

static int dump_head_fd(int fd, int n)
{
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
	return 0;
}

static int dump_head_path(const char *path, int n)
{
	int fd = open(path, 0);
	if (fd < 0) { printf("head: cannot open '%s'\n", path); return -1; }
	int r = dump_head_fd(fd, n);
	close(fd);
	return r;
}

int main(int argc, char **argv)
{
	int n = 10;
	int i = 1;
	if (i < argc && !strcmp(argv[i], "-n") && i + 1 < argc) {
		n = parse_int(argv[i + 1]);
		i += 2;
	}
	if (i >= argc) {
		/* No file argument -- read stdin.  Same as `head -` on
		 * real Unix; the explicit `-` spelling works too. */
		return dump_head_fd(0, n) < 0 ? 1 : 0;
	}

	int err = 0;
	for (; i < argc; i++) {
		int r;
		if (!strcmp(argv[i], "-")) r = dump_head_fd(0, n);
		else                        r = dump_head_path(argv[i], n);
		if (r < 0) err = 1;
	}
	return err;
}

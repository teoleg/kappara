/*
 * cmd/tail.c -- print the last N lines of each named file.
 *
 * Two-pass implementation: first pass counts total lines, second
 * pass prints starting from the (total - N)th '\n'.  Reads the
 * whole file twice -- fine for small files, would obviously want
 * a ring buffer once we have multi-MB files.
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

static int dump_tail(const char *path, int n)
{
	int fd = open(path, 0);
	if (fd < 0) { printf("tail: cannot open '%s'\n", path); return -1; }
	char buf[1024];
	long r;

	/* Pass 1: count lines. */
	int total = 0;
	int last_was_nl = 1;
	while ((r = read(fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < r; i++) {
			if (buf[i] == '\n') total++;
			last_was_nl = (buf[i] == '\n');
		}
	}
	if (!last_was_nl) total++;
	close(fd);

	int skip = total > n ? total - n : 0;
	fd = open(path, 0);
	if (fd < 0) return -1;
	int seen = 0;
	int started = (skip == 0);
	while ((r = read(fd, buf, sizeof(buf))) > 0) {
		long out_start = 0;
		for (long i = 0; i < r; i++) {
			if (!started) {
				if (buf[i] == '\n') {
					seen++;
					if (seen == skip) {
						started = 1;
						out_start = i + 1;
					}
				}
			}
		}
		if (started) {
			long out_end = r;
			write(1, buf + out_start, (size_t)(out_end - out_start));
		}
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
	if (i >= argc) { puts("usage: tail [-n N] <file> [...]"); return 1; }

	int err = 0;
	for (; i < argc; i++)
		if (dump_tail(argv[i], n) < 0) err = 1;
	return err;
}

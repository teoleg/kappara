/*
 * cmd/wc.c -- newline / word / byte counts per file, plus a
 * totals row when more than one file is given.  No flags yet; we
 * always print all three counts in fixed-width columns.
 */

#include <stdio.h>
#include <unistd.h>

struct counts { long l, w, b; };

static int count_file(const char *path, struct counts *c)
{
	int fd = open(path, 0);
	if (fd < 0) { printf("wc: cannot open '%s'\n", path); return -1; }
	char buf[1024];
	long r;
	int  in_word = 0;
	c->l = c->w = c->b = 0;
	while ((r = read(fd, buf, sizeof(buf))) > 0) {
		c->b += r;
		for (long i = 0; i < r; i++) {
			char x = buf[i];
			if (x == '\n') c->l++;
			int is_space = (x == ' ' || x == '\t'
			             || x == '\n' || x == '\r');
			if (is_space) {
				in_word = 0;
			} else if (!in_word) {
				in_word = 1;
				c->w++;
			}
		}
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { puts("usage: wc <file> [...]"); return 1; }

	struct counts tot = { 0, 0, 0 };
	int err = 0;
	for (int i = 1; i < argc; i++) {
		struct counts c;
		if (count_file(argv[i], &c) < 0) { err = 1; continue; }
		printf("%7ld %7ld %7ld %s\n", c.l, c.w, c.b, argv[i]);
		tot.l += c.l; tot.w += c.w; tot.b += c.b;
	}
	if (argc > 2)
		printf("%7ld %7ld %7ld total\n", tot.l, tot.w, tot.b);
	return err;
}

/*
 * cmd/grep.c -- fixed-string grep, one pattern, one or more files.
 *
 * Reads each file line-by-line into a small buffer and runs
 * memmem-style search for the pattern as a literal substring.
 * Lines are bounded at 1024 bytes; longer lines get truncated for
 * the search but still echoed.
 *
 * Multi-file invocation prefixes each match with `filename:` like
 * GNU grep so output is grep-pipeline-friendly even though we
 * don't have pipes wired through the shell yet.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int contains(const char *line, size_t llen,
                    const char *pat,  size_t plen)
{
	if (plen == 0) return 1;
	if (llen < plen) return 0;
	for (size_t i = 0; i + plen <= llen; i++) {
		size_t j = 0;
		while (j < plen && line[i + j] == pat[j]) j++;
		if (j == plen) return 1;
	}
	return 0;
}

static int grep_file(const char *pat, const char *path, int prefix)
{
	int fd = open(path, 0);
	if (fd < 0) { printf("grep: cannot open '%s'\n", path); return -1; }
	size_t plen = strlen(pat);
	char  buf[1024];
	long  r;
	char  line[1024];
	size_t lpos = 0;
	int   matched = 0;
	while ((r = read(fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < r; i++) {
			char c = buf[i];
			if (c == '\n') {
				if (contains(line, lpos, pat, plen)) {
					matched = 1;
					if (prefix) {
						write(1, path, strlen(path));
						write(1, ":", 1);
					}
					write(1, line, lpos);
					write(1, "\n", 1);
				}
				lpos = 0;
			} else if (lpos + 1 < sizeof(line)) {
				line[lpos++] = c;
			}
		}
	}
	/* Trailing partial line without newline. */
	if (lpos > 0 && contains(line, lpos, pat, plen)) {
		matched = 1;
		if (prefix) {
			write(1, path, strlen(path));
			write(1, ":", 1);
		}
		write(1, line, lpos);
		write(1, "\n", 1);
	}
	close(fd);
	return matched ? 0 : 1;	/* grep exit: 0=match, 1=none */
}

int main(int argc, char **argv)
{
	if (argc < 3) { puts("usage: grep <pattern> <file> [...]"); return 2; }
	const char *pat = argv[1];
	int prefix = (argc > 3);	/* multiple files -> filename: prefix */
	int any = 1;	/* 1 = no match anywhere yet */
	for (int i = 2; i < argc; i++) {
		int rc = grep_file(pat, argv[i], prefix);
		if (rc == 0) any = 0;
		/* -1 (open error) keeps any=1 */
	}
	return any;
}

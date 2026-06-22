/*
 * cmd/mv.c -- `mv <src> <dst>`.  We don't have a rename(2)
 * equivalent yet, so this is "copy then unlink" -- correct for
 * regular files, would lose track of directories or active
 * hardlinks (we have neither).
 */

#include <stdio.h>
#include <unistd.h>

static int copy_file(const char *src, const char *dst)
{
	int sfd = open(src, 0);
	if (sfd < 0) { printf("mv: cannot open '%s'\n", src); return -1; }
	(void)creat(dst);
	int dfd = open(dst, 0);
	if (dfd < 0) {
		printf("mv: cannot open '%s'\n", dst);
		close(sfd);
		return -1;
	}
	char buf[1024];
	long n;
	while ((n = read(sfd, buf, sizeof(buf))) > 0) {
		long w = write(dfd, buf, (size_t)n);
		if (w != n) { close(sfd); close(dfd); return -1; }
	}
	close(sfd);
	close(dfd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		puts("usage: mv <src> <dst>");
		return 1;
	}
	if (copy_file(argv[1], argv[2]) < 0)
		return 1;
	if (unlink(argv[1]) < 0) {
		printf("mv: copied but couldn't unlink src '%s'\n", argv[1]);
		return 1;
	}
	return 0;
}

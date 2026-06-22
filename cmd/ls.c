/*
 * cmd/ls.c -- minimal `ls` replacement, the standalone version.
 *
 * Reads a directory listing through the SYS_ls syscall (which returns
 * names separated by '\n', NUL-terminated) and prints them one per
 * line.  Default path is "/" because we don't have per-process cwd
 * yet -- a future change adds chdir/getcwd and SYS_ls + sibling
 * syscalls will resolve relative paths against vm_map->cwd.
 *
 * See docs/SHELL.md for the migration to standalone.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *path = (argc >= 2) ? argv[1] : ".";

	char buf[1024];
	long n = ls(path, buf, sizeof(buf));
	if (n < 0) {
		printf("ls: cannot access '%s'\n", path);
		return 1;
	}

	/* sys_ls fills `buf` with names separated by '\n'.  Print as-is;
	 * if the buffer didn't end with '\n' (truncated mid-name), add
	 * one so the prompt lands on a clean column. */
	write(1, buf, (size_t)n);
	if (n > 0 && buf[n - 1] != '\n')
		write(1, "\n", 1);
	return 0;
}

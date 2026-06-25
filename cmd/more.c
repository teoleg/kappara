/*
 * cmd/more.c -- paginate input one screenful at a time.
 *
 * Usage:
 *   more <file> [...]    -- page through each named file
 *   more                 -- page through stdin (e.g. `cat foo | more`)
 *
 * After each page the program prompts "--More--" and waits for one
 * keystroke on the controlling tty:
 *   SPACE / ENTER  -- next page
 *   q              -- quit
 *
 * Keys are read from /dev/tty0, NOT from the pipe -- otherwise
 * `cat foo | more` couldn't tell user input from input data.  That's
 * the same trick real more(1) does (it opens /dev/tty).
 *
 * Page size: fixed 20 lines.  Real more queries termios for the
 * window size; kappara has no SIGWINCH or stty -y plumbing yet.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PAGE_LINES	20

static int tty_fd = -1;

/* Lazy-open the controlling tty for the prompt-reply read.  We
 * keep stdin (fd 0) untouched because it might be the pipe. */
static int tty_open(void)
{
	if (tty_fd >= 0) return tty_fd;
	tty_fd = (int)open("/dev/tty0", 0);
	return tty_fd;
}

/* Show the prompt, read one byte from the tty.  Returns:
 *    0  = continue (space / enter / anything else)
 *   -1  = quit (q / Q / EOF on the tty) */
static int prompt_continue(void)
{
	write(1, "--More--", 8);
	int fd = tty_open();
	if (fd < 0) return -1;
	char c;
	long n = read(fd, &c, 1);
	/* Erase the "--More--" prompt by overwriting with spaces + CR
	 * so the next page starts at column 0 without the artifact. */
	write(1, "\r        \r", 10);
	if (n != 1) return -1;
	if (c == 'q' || c == 'Q') return -1;
	return 0;
}

static int page_fd(int fd)
{
	char buf[1024];
	int  lines = 0;
	long r;
	while ((r = read(fd, buf, sizeof(buf))) > 0) {
		long out = 0;
		for (long i = 0; i < r; i++) {
			if (buf[i] == '\n') {
				lines++;
				if (lines == PAGE_LINES) {
					write(1, buf + out, (size_t)(i + 1 - out));
					out = i + 1;
					if (prompt_continue() < 0) return 1;
					lines = 0;
				}
			}
		}
		if (out < r) write(1, buf + out, (size_t)(r - out));
	}
	return 0;
}

static int page_path(const char *path)
{
	int fd = open(path, 0);
	if (fd < 0) {
		write(2, "more: cannot open '", 19);
		write(2, path, (size_t)strlen(path));
		write(2, "'\n", 2);
		return -1;
	}
	int r = page_fd(fd);
	close(fd);
	return r;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		/* No file argument -- page stdin.  Matches the
		 * `cat foo | more` use case. */
		return page_fd(0);
	}
	int rc = 0;
	for (int i = 1; i < argc; i++) {
		int r;
		if (!strcmp(argv[i], "-")) r = page_fd(0);
		else                        r = page_path(argv[i]);
		if (r < 0) rc = 1;
		if (r > 0) break;	/* user hit q */
	}
	if (tty_fd >= 0) close(tty_fd);
	return rc;
}

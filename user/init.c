/*
 * user/init.c -- kappara's first userspace process: the shell
 *
 * Pid 1.  Runs entirely in EL0; every keystroke and every output
 * byte goes through a syscall.  This file used to be a 30-line "log
 * hello and yield" demo; it's now a real interactive shell, the
 * successor to the kernel-mode ksh.c that lived under kernel/.
 *
 * Boot flow
 * ---------
 *   user_spawn() (kernel/user.c) creates a kthread whose first run
 *   eret's into EL0 at USER_VA (= 0x10000000), which is _start
 *   below (forced first by user/linker.ld).  _start opens
 *   /dev/console, prints a welcome line, and falls into the
 *   read-line / parse / dispatch loop.
 *
 * I/O model
 * ---------
 *   The shell holds two file descriptors throughout its life:
 *     fd_console -- opened on /dev/console; used for input and
 *                   echoing typed characters back, and for any
 *                   command output.  The first open of /dev/console
 *                   becomes console_active in the kernel, so the
 *                   uart_rx kthread feeds bytes into our read queue.
 *     fd_user    -- a "current" fd updated by `open`; commands like
 *                   `read`, `write`, `push`, `pop`, `close` operate
 *                   on this one.
 *
 *   Echo is implemented as sys_write of one byte at a time on
 *   fd_console.  Yes, that's one SVC per typed character; for an
 *   interactive shell at human speed it's fine.
 *
 * Commands
 * --------
 *   help / pid / ls / open / close / read / write / push / pop
 *   (No `tree` -- the kernel-only vfs_dump_tree isn't exposed via
 *   syscall yet.)
 */

#include "syscall.h"

/* -------- tiny libc-substitute -------- */

static size_t ustrlen(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	return n;
}

static int ustrcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static char *udec(char *dst, long v)
{
	char tmp[24];
	int n = 0;
	if (v == 0) { tmp[n++] = '0'; }
	else {
		long x = v < 0 ? -v : v;
		while (x) { tmp[n++] = (char)('0' + (x % 10)); x /= 10; }
		if (v < 0) tmp[n++] = '-';
	}
	while (n--) *dst++ = tmp[n];
	*dst = '\0';
	return dst;
}

static long uparse_long(const char *s)
{
	long v = 0;
	int neg = 0;
	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
	return neg ? -v : v;
}

/* -------- shell state -------- */

static int  fd_console = -1;
static int  fd_user    = -1;
static char cwd[128]   = "/";

#define LINE_MAX	128
#define TOK_MAX		8
#define HIST_MAX	8	/* recent command lines kept for arrow-up */

static char hist[HIST_MAX][LINE_MAX];
static int  hist_count;	/* 0..HIST_MAX -- how many slots are filled */

/* Combine cwd + arg into out so commands accept relative paths.
 * Absolute paths (start with '/') pass through. */
/* Canonicalize an absolute path in place: strip "." segments, pop on
 * "..", collapse "//" runs.  The result always starts with '/' and
 * has no trailing slash (except the root itself).  Done with a small
 * starts[] stack so we can pop a segment without rescanning. */
static void path_canon(char *p)
{
	if (p[0] != '/') return;

	char   out[128];
	size_t starts[32];
	int    nseg = 0;
	size_t o = 1;
	out[0] = '/';

	size_t i = 1;
	for (;;) {
		while (p[i] == '/') i++;
		if (!p[i]) break;
		size_t start = i;
		while (p[i] && p[i] != '/') i++;
		size_t len = i - start;

		if (len == 1 && p[start] == '.') {
			continue;
		}
		if (len == 2 && p[start] == '.' && p[start + 1] == '.') {
			if (nseg > 0) {
				nseg--;
				o = starts[nseg];
			}
			continue;
		}
		if (nseg >= 32 || o + len + 2 >= sizeof(out)) break;
		starts[nseg++] = o;
		for (size_t k = 0; k < len; k++) out[o++] = p[start + k];
		out[o++] = '/';
	}
	if (o > 1 && out[o - 1] == '/') o--;	/* drop trailing slash */
	out[o] = '\0';

	for (size_t k = 0; k <= o; k++) p[k] = out[k];
}

static void resolve_path(const char *arg, char *out, size_t cap)
{
	if (arg[0] == '/') {
		size_t i = 0;
		while (arg[i] && i + 1 < cap) { out[i] = arg[i]; i++; }
		out[i] = '\0';
	} else {
		size_t i = 0;
		while (cwd[i] && i + 1 < cap) { out[i] = cwd[i]; i++; }
		/* Add a slash separator unless cwd is "/" (already ends in '/'). */
		if (i > 1 && i + 1 < cap) out[i++] = '/';
		size_t j = 0;
		while (arg[j] && i + 1 < cap) out[i++] = arg[j++];
		out[i] = '\0';
	}
	path_canon(out);
}

/* -------- console output helpers -------- */

static void cwriten(const char *s, size_t n)
{
	sys_write(fd_console, s, n);
}

static void cwrite(const char *s)
{
	cwriten(s, ustrlen(s));
}

static void cputc(char c)
{
	cwriten(&c, 1);
}

static void cprint_long(long v)
{
	char buf[24];
	udec(buf, v);
	cwrite(buf);
}

/* -------- line input via sys_read on fd_console -------- */

static void prompt(void)
{
	cwrite("kappara:"); cwrite(cwd); cwrite("# ");
}

static int read_one(void)
{
	char c;
	long n = sys_read(fd_console, &c, 1);
	if (n != 1)
		return -1;
	return (unsigned char)c;
}

/* Push line onto the history ring (hist[0] = most recent).  No-op on
 * empty input or exact duplicate of the previous entry. */
static void history_add(const char *line)
{
	if (!line[0]) return;
	if (hist_count > 0 && !ustrcmp(line, hist[0])) return;

	int n = hist_count < HIST_MAX ? hist_count : HIST_MAX - 1;
	for (int i = n - 1; i >= 0; i--)
		for (int j = 0; j < LINE_MAX; j++)
			hist[i + 1][j] = hist[i][j];
	size_t j = 0;
	while (line[j] && j < LINE_MAX - 1) { hist[0][j] = line[j]; j++; }
	hist[0][j] = '\0';
	if (hist_count < HIST_MAX) hist_count++;
}

/* Replace the displayed line with `new_line` -- ANSI CR + clear-EOL,
 * redraw the prompt, then write the new bytes.  Updates *i_inout to
 * reflect the new content length. */
static void replace_line(char *buf, size_t *i_inout, const char *new_line)
{
	cwrite("\r\033[K");
	prompt();
	size_t j = 0;
	while (new_line[j] && j < LINE_MAX - 1) {
		buf[j] = new_line[j];
		cputc(new_line[j]);
		j++;
	}
	buf[j] = '\0';
	*i_inout = j;
}

/*
 * Line input with VT100 cursor-key support.  After reading a byte
 * we feed it through a tiny state machine:
 *   ESC               -> set in_esc
 *   in_esc and '['    -> set in_csi (CSI = Control Sequence Introducer)
 *   in_csi and 'A'    -> up arrow  -> previous history entry
 *   in_csi and 'B'    -> down arrow -> next entry / blank
 * All other bytes go through the normal echo+buffer path.
 */
/*
 * Set by the SIGINT handler (installed in _start) when the user hits
 * Ctrl-C.  read_line checks it on every loop iteration, clears the
 * partial input, and re-prompts -- so the shell feels like bash:
 * type "ec" Ctrl-C, line gone, fresh prompt.
 */
static volatile int sigint_pending;

__attribute__((used))
static void sigint_handler(int sig)
{
	(void)sig;
	cwrite("^C\r\n");
	sigint_pending = 1;
}

/*
 * Editors (ked / vi / kc) share the shell's address space, so the
 * shell's SIGINT handler would draw "^C" over their display if the
 * user hits Ctrl-C mid-edit.  These helpers install SIG_IGN for the
 * duration of an edit and put the original handler back afterwards.
 * sigint_pending is also cleared on enter (in case the shell saw a
 * Ctrl-C that hadn't been observed yet) and on exit (any Ctrl-Cs that
 * landed during the edit are intentionally swallowed -- the editor
 * runs to its own quit command).
 */
static void editor_suspend_sigint(struct sigaction *save)
{
	struct sigaction ign;
	ign.sa_handler = SIG_IGN_PTR;
	ign.sa_mask    = 0;
	ign.sa_flags   = 0;
	sys_sigaction(SIGINT, &ign, save);
	sigint_pending = 0;
}

static void editor_restore_sigint(const struct sigaction *save)
{
	sys_sigaction(SIGINT, save, 0);
	sigint_pending = 0;
}

static void read_line(char *buf, size_t cap)
{
	size_t i = 0;
	int hist_pos = -1;	/* -1 = currently editing a fresh line */
	int in_esc = 0;
	int in_csi = 0;

	for (;;) {
		if (sigint_pending) {
			sigint_pending = 0;
			i = 0;
			hist_pos = -1;
			prompt();
		}
		int c = read_one();
		if (c < 0) { sys_yield(); continue; }

		if (in_csi) {
			in_csi = 0;
			if (c == 'A') {		/* up */
				if (hist_pos + 1 < hist_count) {
					hist_pos++;
					replace_line(buf, &i, hist[hist_pos]);
				}
			} else if (c == 'B') {	/* down */
				if (hist_pos > 0) {
					hist_pos--;
					replace_line(buf, &i, hist[hist_pos]);
				} else if (hist_pos == 0) {
					hist_pos = -1;
					replace_line(buf, &i, "");
				}
			}
			continue;
		}
		if (in_esc) {
			in_esc = 0;
			if (c == '[') { in_csi = 1; continue; }
			continue;
		}
		if (c == 0x1B) { in_esc = 1; continue; }

		if (c == '\r' || c == '\n') {
			cwrite("\r\n");
			break;
		}
		if (c == 0x08 || c == 0x7F) {
			if (i > 0) { i--; cwrite("\b \b"); }
			continue;
		}
		if (i + 1 < cap) {
			buf[i++] = (char)c;
			cputc((char)c);
		}
	}
	buf[i] = '\0';
	history_add(buf);
}

/* -------- tokenize on spaces -------- */

static int tokenize(char *line, char *argv[], int max)
{
	int argc = 0;
	char *p = line;
	while (*p && argc < max) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t') p++;
		if (*p) { *p = '\0'; p++; }
	}
	return argc;
}

/* -------- command implementations -------- */

static void cmd_help(void)
{
	cwrite("commands:\r\n"
		"  help\r\n"
		"  pid                    sys_getpid\r\n"
		"  pwd                    print current directory\r\n"
		"  cd [path]              change current directory\r\n"
		"  ls [path]              list a directory\r\n"
		"  lsl [path]             list with type + size (ls -l)\r\n"
		"  open <path>            open and remember as $fd\r\n"
		"  close                  close $fd\r\n"
		"  read [n]               read up to n bytes from $fd\r\n"
		"  write <text...>        write text to $fd\r\n"
		"  push <module>          ioctl(I_PUSH) on $fd\r\n"
		"  pop                    ioctl(I_POP) on $fd\r\n"
		"  cat <path>             dump file to console\r\n"
		"  echo <path> <text>     write text to file (overwrite)\r\n"
		"  ked <path>             tiny ed-like line editor\r\n"
		"  vi <path>              modal full-screen editor (vi-lite)\r\n"
		"  kc                     two-panel file manager (Norton-Commander-style)\r\n"
		"  touch <path>           create empty file (kfs only)\r\n"
		"  mkdir <path>           create directory (kfs only)\r\n"
		"  rm <path>              remove file\r\n"
		"  rmdir <path>           remove empty directory\r\n"
		"  append <path> <text>   append text + newline to file\r\n"
		"  spawn [arg]            sys_spawn a worker thread\r\n"
		"  kill <tid> [sig]       POSIX signal numbers (default SIGTERM=15)\r\n"
		"  exec <path>            load & run an ELF from /bin (or any path)\r\n"
		"  halt                   ask QEMU to exit (semihosting)\r\n"
		"  ftrace [on|off|reset|dump]  per-CPU function tracer\r\n"
		"  <name>                 unknown commands look up /usr/bin/<name>\r\n"
		"                         (ps, sigtest, masktest, waittest, segvtest,\r\n"
		"                          crash, pipe, pipework)\r\n");
}

static void cmd_pid(void)
{
	cprint_long(sys_getpid());
	cwrite("\r\n");
}

static void cmd_ls(int argc, char *argv[])
{
	char path[128];
	if (argc > 1)
		resolve_path(argv[1], path, sizeof(path));
	else {
		size_t i = 0;
		while (cwd[i]) { path[i] = cwd[i]; i++; }
		path[i] = '\0';
	}
	char out[256];
	long n = sys_ls(path, out, sizeof(out));
	if (n < 0) {
		cwrite("ls: cannot access '"); cwrite(path); cwrite("'\r\n");
		return;
	}
	for (long i = 0; i < n; i++) {
		if (out[i] == '\n') cputc('\r');
		cputc(out[i]);
	}
}

static void cmd_lsl(int argc, char *argv[])
{
	char path[128];
	if (argc > 1)
		resolve_path(argv[1], path, sizeof(path));
	else {
		size_t i = 0;
		while (cwd[i]) { path[i] = cwd[i]; i++; }
		path[i] = '\0';
	}
	char out[512];
	long n = sys_lsl(path, out, sizeof(out));
	if (n < 0) {
		cwrite("lsl: cannot access '"); cwrite(path); cwrite("'\r\n");
		return;
	}
	for (long i = 0; i < n; i++) {
		if (out[i] == '\n') cputc('\r');
		cputc(out[i]);
	}
}

static void cmd_cd(int argc, char *argv[])
{
	const char *target = (argc > 1) ? argv[1] : "/";
	char path[128];
	resolve_path(target, path, sizeof(path));
	/* Probe before mutating cwd: sys_ls returns -1 for both a
	 * non-existent path and a regular file (vfs_listdir rejects
	 * anything that isn't INODE_DIR), so a single call covers
	 * "no such directory" without needing a stat syscall. */
	char probe[1];
	if (sys_ls(path, probe, sizeof(probe)) < 0) {
		cwrite("cd: no such directory: ");
		cwrite(path); cwrite("\r\n");
		return;
	}
	size_t i = 0;
	while (path[i] && i + 1 < sizeof(cwd)) { cwd[i] = path[i]; i++; }
	cwd[i] = '\0';
}

static void cmd_pwd(void)
{
	cwrite(cwd); cwrite("\r\n");
}

static void cmd_touch(int argc, char *argv[])
{
	if (argc < 2) { cwrite("usage: touch <path>\r\n"); return; }
	char path[128];
	resolve_path(argv[1], path, sizeof(path));
	long r = sys_creat(path);
	if (r < 0) {
		cwrite("touch: failed: "); cwrite(path); cwrite("\r\n");
	}
}

static void cmd_mkdir(int argc, char *argv[])
{
	if (argc < 2) { cwrite("usage: mkdir <path>\r\n"); return; }
	char path[128];
	resolve_path(argv[1], path, sizeof(path));
	long r = sys_mkdir(path);
	if (r < 0) {
		cwrite("mkdir: failed: "); cwrite(path); cwrite("\r\n");
	}
}

static void cmd_rm(int argc, char *argv[])
{
	if (argc < 2) { cwrite("usage: rm <path>\r\n"); return; }
	char path[128];
	resolve_path(argv[1], path, sizeof(path));
	long r = sys_unlink(path);
	if (r < 0) {
		cwrite("rm: failed: "); cwrite(path); cwrite("\r\n");
	}
}

static void cmd_rmdir(int argc, char *argv[])
{
	if (argc < 2) { cwrite("usage: rmdir <path>\r\n"); return; }
	char path[128];
	resolve_path(argv[1], path, sizeof(path));
	long r = sys_rmdir(path);
	if (r < 0) {
		cwrite("rmdir: failed (not empty?): ");
		cwrite(path); cwrite("\r\n");
	}
}

static void cmd_append(int argc, char *argv[])
{
	if (argc < 3) { cwrite("usage: append <path> <text...>\r\n"); return; }
	char path[128];
	resolve_path(argv[1], path, sizeof(path));
	long fd = sys_open(path, 0);
	if (fd < 0) { cwrite("append: cannot open '");
		      cwrite(path); cwrite("'\r\n"); return; }
	if (sys_seek((int)fd, 0, SEEK_END) < 0) {
		cwrite("append: seek failed\r\n");
		sys_close((int)fd); return;
	}
	char buf[256];
	size_t off = 0;
	for (int i = 2; i < argc; i++) {
		if (i > 2 && off + 1 < sizeof(buf)) buf[off++] = ' ';
		size_t l = ustrlen(argv[i]);
		if (off + l > sizeof(buf)) l = sizeof(buf) - off;
		for (size_t j = 0; j < l; j++) buf[off + j] = argv[i][j];
		off += l;
	}
	if (off + 1 < sizeof(buf)) buf[off++] = '\n';
	long w = sys_write((int)fd, buf, off);
	cwrite("wrote "); cprint_long(w); cwrite(" bytes\r\n");
	sys_close((int)fd);
}

static void cmd_open(int argc, char *argv[])
{
	if (argc < 2) { cwrite("usage: open <path>\r\n"); return; }
	if (fd_user >= 0) { sys_close(fd_user); fd_user = -1; }
	char path[128];
	resolve_path(argv[1], path, sizeof(path));
	long fd = sys_open(path, 0);
	if (fd < 0) { cwrite("open: failed\r\n"); return; }
	fd_user = (int)fd;
	cwrite("fd="); cprint_long(fd); cwrite("\r\n");
}

static void cmd_close(void)
{
	if (fd_user < 0) { cwrite("no open fd\r\n"); return; }
	sys_close(fd_user);
	fd_user = -1;
}

static void cmd_read(int argc, char *argv[])
{
	if (fd_user < 0) { cwrite("no open fd\r\n"); return; }
	long want = (argc > 1) ? uparse_long(argv[1]) : 128;
	if (want <= 0 || want > 256) want = 128;
	char buf[256];
	long n = sys_read(fd_user, buf, (size_t)want);
	if (n <= 0) {
		cwrite("read: ");
		cprint_long(n);
		cwrite("\r\n");
		return;
	}
	for (long i = 0; i < n; i++) {
		if (buf[i] == '\n') cputc('\r');
		cputc(buf[i]);
	}
	if (n > 0 && buf[n - 1] != '\n')
		cwrite("\r\n");
}

static void cmd_write(int argc, char *argv[])
{
	if (fd_user < 0) { cwrite("no open fd\r\n"); return; }
	if (argc < 2) { cwrite("usage: write <text>\r\n"); return; }
	char buf[256];
	size_t off = 0;
	for (int i = 1; i < argc; i++) {
		if (i > 1 && off + 1 < sizeof(buf))
			buf[off++] = ' ';
		size_t l = ustrlen(argv[i]);
		if (off + l > sizeof(buf)) l = sizeof(buf) - off;
		for (size_t j = 0; j < l; j++) buf[off + j] = argv[i][j];
		off += l;
	}
	long w = sys_write(fd_user, buf, off);
	cprint_long(w); cwrite(" bytes\r\n");
}

static void cmd_push(int argc, char *argv[])
{
	if (fd_user < 0) { cwrite("no open fd\r\n"); return; }
	if (argc < 2) { cwrite("usage: push <module>\r\n"); return; }
	long r = sys_ioctl(fd_user, I_PUSH, (long)(unsigned long)argv[1]);
	cwrite("push: "); cprint_long(r); cwrite("\r\n");
}

static void cmd_pop(void)
{
	if (fd_user < 0) { cwrite("no open fd\r\n"); return; }
	long r = sys_ioctl(fd_user, I_POP, 0);
	cwrite("pop: "); cprint_long(r); cwrite("\r\n");
}

static void cmd_cat(int argc, char *argv[])
{
	if (argc < 2) { cwrite("usage: cat <path>\r\n"); return; }
	char path[128];
	resolve_path(argv[1], path, sizeof(path));
	long fd = sys_open(path, 0);
	if (fd < 0) { cwrite("cat: cannot open '"); cwrite(path);
		      cwrite("'\r\n"); return; }
	char buf[256];
	long n;
	while ((n = sys_read((int)fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < n; i++) {
			if (buf[i] == '\n') cputc('\r');
			cputc(buf[i]);
		}
	}
	sys_close((int)fd);
}

static void cmd_echo(int argc, char *argv[])
{
	if (argc < 3) { cwrite("usage: echo <path> <text...>\r\n"); return; }
	char path[128];
	resolve_path(argv[1], path, sizeof(path));
	long fd = sys_open(path, O_TRUNC);
	if (fd < 0) { cwrite("echo: cannot open '"); cwrite(path);
		      cwrite("'\r\n"); return; }

	char buf[256];
	size_t off = 0;
	for (int i = 2; i < argc; i++) {
		if (i > 2 && off + 1 < sizeof(buf))
			buf[off++] = ' ';
		size_t l = ustrlen(argv[i]);
		if (off + l > sizeof(buf)) l = sizeof(buf) - off;
		for (size_t j = 0; j < l; j++) buf[off + j] = argv[i][j];
		off += l;
	}
	if (off + 1 < sizeof(buf)) buf[off++] = '\n';

	long w = sys_write((int)fd, buf, off);
	cwrite("wrote "); cprint_long(w); cwrite(" bytes\r\n");
	sys_close((int)fd);
}

/* -------- ked: a tiny ed-like line editor -------- */

/*
 * ked: tiny line editor in the spirit of Unix ed(1).
 *
 * The buffer is a fixed 2-D char array (no malloc in userland).  Line
 * numbers are 1-based; line 0 means "before the first line" -- used
 * by `0a` to append at the very top.  `$` and `.` are addresses for
 * "last line" and "current line" respectively, the same ed shorthand.
 *
 * Commands (only what's actually useful)
 * --------------------------------------
 *   <addr>            move to addr and print that line
 *   p, <range>p       print line(s); `,p` is `1,$p`
 *   a, <addr>a        append after addr (or current); end with a "." line
 *   i, <addr>i        insert before addr
 *   d, <range>d       delete line(s)
 *   w                 write back to the file
 *   q                 quit (refuses if dirty)
 *   q!                force quit
 *   empty             advance one line and print
 *
 * The split between command-mode read (one line at the ed prompt) and
 * insert-mode read (text lines until ".") both use simple_read_line --
 * no history, no arrow keys -- because that's what ed feels like and
 * we don't want stray VT100 escapes ending up in the file.
 */

#define KED_MAX_LINES	64
#define KED_MAX_LINE	120

static struct {
	char	path[64];
	char	lines[KED_MAX_LINES][KED_MAX_LINE];
	int	nlines;
	int	cur;	/* 1..nlines; 0 means "no current line" (empty buf) */
	int	dirty;
} ked;

/* Bare-bones line read: backspace works, no history, no escape parsing.
 * Suitable for ed insert mode where stray VT100 sequences would land
 * in the file, and for the ed command line itself. */
static void simple_read_line(char *buf, size_t cap)
{
	size_t i = 0;
	for (;;) {
		int c = read_one();
		if (c < 0) { sys_yield(); continue; }
		if (c == '\r' || c == '\n') { cwrite("\r\n"); break; }
		if (c == 0x08 || c == 0x7F) {
			if (i > 0) { i--; cwrite("\b \b"); }
			continue;
		}
		if (i + 1 < cap) {
			buf[i++] = (char)c;
			cputc((char)c);
		}
	}
	buf[i] = '\0';
}

static void ked_copy_str(char *dst, const char *src, size_t cap)
{
	size_t i = 0;
	for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
	dst[i] = '\0';
}

static int ked_load(const char *path)
{
	ked.nlines = 0;
	ked.cur    = 0;
	ked.dirty  = 0;
	ked_copy_str(ked.path, path, sizeof(ked.path));

	long fd = sys_open(path, 0);
	if (fd < 0) {
		/* Treat as a new file -- empty buffer is fine. */
		return 0;
	}
	char buf[256];
	int  line_idx = 0;
	int  col      = 0;
	long n;
	while ((n = sys_read((int)fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < n; i++) {
			char c = buf[i];
			if (c == '\n') {
				if (line_idx < KED_MAX_LINES) {
					ked.lines[line_idx][col] = '\0';
					line_idx++;
				}
				col = 0;
			} else if (col < KED_MAX_LINE - 1
				   && line_idx < KED_MAX_LINES) {
				ked.lines[line_idx][col++] = c;
			}
		}
	}
	/* File without trailing newline -- keep the last partial line. */
	if (col > 0 && line_idx < KED_MAX_LINES) {
		ked.lines[line_idx][col] = '\0';
		line_idx++;
	}
	sys_close((int)fd);
	ked.nlines = line_idx;
	ked.cur    = ked.nlines;	/* ed: addr after load = last line */
	return 0;
}

static int ked_save(void)
{
	long fd = sys_open(ked.path, O_TRUNC);
	if (fd < 0) {
		/* File didn't exist -- create it then reopen with TRUNC. */
		if (sys_creat(ked.path) < 0) {
			cwrite("ked: cannot create '");
			cwrite(ked.path); cwrite("'\r\n");
			return -1;
		}
		fd = sys_open(ked.path, O_TRUNC);
		if (fd < 0) return -1;
	}
	long total = 0;
	for (int i = 0; i < ked.nlines; i++) {
		size_t len = ustrlen(ked.lines[i]);
		sys_write((int)fd, ked.lines[i], len);
		sys_write((int)fd, "\n", 1);
		total += (long)len + 1;
	}
	sys_close((int)fd);
	ked.dirty = 0;
	cprint_long(total); cwrite(" bytes\r\n");
	return 0;
}

static void ked_print_range(int from, int to)
{
	if (from < 1) from = 1;
	if (to > ked.nlines) to = ked.nlines;
	for (int i = from; i <= to; i++) {
		cprint_long(i); cwrite(": ");
		cwrite(ked.lines[i - 1]);
		cwrite("\r\n");
	}
	if (to >= from) ked.cur = to;
}

/* Read insert-mode lines until a lone "." and splice them in after
 * the given address (0 = at the very top). */
static void ked_insert_after(int after)
{
	for (;;) {
		char line[KED_MAX_LINE];
		simple_read_line(line, sizeof(line));
		if (line[0] == '.' && line[1] == '\0') break;
		if (ked.nlines >= KED_MAX_LINES) {
			cwrite("ked: buffer full\r\n"); break;
		}
		for (int i = ked.nlines; i > after; i--)
			ked_copy_str(ked.lines[i], ked.lines[i - 1],
				     KED_MAX_LINE);
		ked_copy_str(ked.lines[after], line, KED_MAX_LINE);
		ked.nlines++;
		after++;
		ked.cur   = after;
		ked.dirty = 1;
	}
}

static void ked_delete_line(int n)
{
	if (n < 1 || n > ked.nlines) return;
	for (int i = n - 1; i < ked.nlines - 1; i++)
		ked_copy_str(ked.lines[i], ked.lines[i + 1], KED_MAX_LINE);
	ked.nlines--;
	ked.dirty = 1;
	if (ked.cur > ked.nlines) ked.cur = ked.nlines;
}

/* Parse a single ed address from *p (numeric, '.', '$').  Returns the
 * resolved line number, or -1 if no address was present.  Advances *p
 * past whatever it consumed. */
static int ked_parse_addr(const char **p)
{
	if (**p == '.') { (*p)++; return ked.cur; }
	if (**p == '$') { (*p)++; return ked.nlines; }
	int n = 0, has = 0;
	while (**p >= '0' && **p <= '9') {
		n = n * 10 + (**p - '0');
		(*p)++;
		has = 1;
	}
	return has ? n : -1;
}

/* Returns 1 if the command was `q` or `q!` and we should exit ked. */
static int ked_dispatch(const char *line)
{
	const char *p = line;
	int from = ked_parse_addr(&p);
	int to   = from;
	if (*p == ',') {
		p++;
		int t = ked_parse_addr(&p);
		to   = (t   < 0) ? ked.nlines : t;
		if (from < 0) from = 1;
	}
	char cmd = *p ? *p++ : '\0';
	int force = 0;
	if (*p == '!') { force = 1; p++; }

	if (cmd == '\0') {
		/* Empty input + no address -> advance one line. */
		int n = (from > 0) ? from : ked.cur + 1;
		if (n < 1 || n > ked.nlines) { cwrite("?\r\n"); return 0; }
		ked.cur = n;
		ked_print_range(n, n);
		return 0;
	}

	switch (cmd) {
	case 'p':
		if (from < 0) from = ked.cur;
		if (to   < 0) to   = from;
		if (from < 1 || from > ked.nlines) { cwrite("?\r\n"); break; }
		ked_print_range(from, to);
		break;
	case 'a':
		ked_insert_after(from > 0 ? from : ked.cur);
		break;
	case 'i': {
		int at = (from > 0) ? from : ked.cur;
		if (at > 0) at--;
		ked_insert_after(at);
		break;
	}
	case 'd':
		if (from < 0) from = ked.cur;
		if (to   < 0) to   = from;
		if (from < 1 || from > ked.nlines) { cwrite("?\r\n"); break; }
		for (int i = to; i >= from; i--) ked_delete_line(i);
		break;
	case 'w':
		ked_save();
		break;
	case 'q':
		if (ked.dirty && !force) {
			cwrite("ked: unsaved changes (use q!)\r\n");
			return 0;
		}
		return 1;
	default:
		cwrite("?\r\n");
		break;
	}
	return 0;
}

static void cmd_ked(int argc, char *argv[])
{
	if (argc < 2) { cwrite("usage: ked <path>\r\n"); return; }
	char path[128];
	resolve_path(argv[1], path, sizeof(path));
	if (ked_load(path) < 0) { cwrite("ked: load failed\r\n"); return; }
	cwrite("ked: "); cwrite(path); cwrite(" (");
	cprint_long(ked.nlines); cwrite(" lines)\r\n");

	struct sigaction saved_sigint;
	editor_suspend_sigint(&saved_sigint);
	for (;;) {
		cwrite("* ");
		char cmd[80];
		simple_read_line(cmd, sizeof(cmd));
		if (ked_dispatch(cmd)) break;
	}
	editor_restore_sigint(&saved_sigint);
}

/* -------- vi: tiny modal editor -------- */

/*
 * vi is a stripped-down modal editor in the spirit of vi(1).  Three
 * modes (NORMAL / INSERT / COMMAND), a fixed line buffer (same shape
 * as ked), a VT100 status bar at the bottom of the screen.
 *
 *   NORMAL  -- hjkl + arrows move; 0/$ jump; gg/G jump file edges;
 *              i/a/I/A open INSERT at various positions; o/O open a
 *              line; x deletes the char under the cursor; dd deletes
 *              the line; : opens COMMAND mode.
 *   INSERT  -- printable chars typed in at the cursor; backspace
 *              eats one; ESC returns to NORMAL.
 *   COMMAND -- after `:`, type `w` to save, `q`/`q!` to quit,
 *              `wq` (or `x`) to save and quit; ESC or Enter on an
 *              empty line cancels.
 *
 * Display assumes a 24x80 VT100-style terminal -- it doesn't query
 * the actual size, just clears the screen and positions glyphs by
 * row/column with `ESC [ row ; col H`.  Status bar pinned at row 24.
 *
 * No undo, no search, no yank/paste, no movement repeats (3w etc).
 * Plenty of headroom for follow-ups.
 */

#define VI_MAX_LINES	64
#define VI_MAX_LINE	120
#define VI_ROWS		23	/* content rows; row 24 is the status bar */
#define VI_COLS		80

enum vi_mode { VI_NORMAL, VI_INSERT, VI_COMMAND };

static struct {
	char	path[64];
	char	lines[VI_MAX_LINES][VI_MAX_LINE];
	int	nlines;
	int	cy;		/* cursor line, 0-based */
	int	cx;		/* cursor column, 0-based */
	int	top;		/* topmost visible line index */
	int	dirty;
	enum vi_mode mode;
	char	cmdbuf[40];	/* COMMAND-mode line being typed */
	int	cmdlen;
	char	status[80];	/* one-shot status message */
} vi;

/* VT100 helpers.  Direct UART path -- we don't want history or echo
 * interfering. */
static void vt_clear_screen(void) { cwrite("\033[2J\033[H"); }
static void vt_move(int row, int col)
{
	/* row/col are 1-based here. */
	cwrite("\033[");
	cprint_long(row);
	cputc(';');
	cprint_long(col);
	cputc('H');
}
static void vt_clear_eol(void) { cwrite("\033[K"); }
static void vt_invert_on(void)  { cwrite("\033[7m"); }
static void vt_invert_off(void) { cwrite("\033[0m"); }

static void vi_copy_str(char *dst, const char *src, size_t cap)
{
	size_t i = 0;
	for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
	dst[i] = '\0';
}

/* Same load path as ked. */
static int vi_load(const char *path)
{
	vi.nlines = 0; vi.cy = vi.cx = vi.top = 0; vi.dirty = 0;
	vi.mode = VI_NORMAL; vi.cmdlen = 0; vi.status[0] = '\0';
	vi_copy_str(vi.path, path, sizeof(vi.path));

	long fd = sys_open(path, 0);
	if (fd < 0) return 0;	/* new file is fine */
	char buf[256];
	int  li = 0, col = 0;
	long n;
	while ((n = sys_read((int)fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < n; i++) {
			char c = buf[i];
			if (c == '\n') {
				if (li < VI_MAX_LINES) {
					vi.lines[li][col] = '\0';
					li++;
				}
				col = 0;
			} else if (col < VI_MAX_LINE - 1
				   && li < VI_MAX_LINES) {
				vi.lines[li][col++] = c;
			}
		}
	}
	if (col > 0 && li < VI_MAX_LINES) {
		vi.lines[li][col] = '\0';
		li++;
	}
	sys_close((int)fd);
	vi.nlines = li;
	if (vi.nlines == 0) {
		vi.lines[0][0] = '\0';
		vi.nlines = 1;
	}
	return 0;
}

static int vi_save(void)
{
	long fd = sys_open(vi.path, O_TRUNC);
	if (fd < 0) {
		if (sys_creat(vi.path) < 0) return -1;
		fd = sys_open(vi.path, O_TRUNC);
		if (fd < 0) return -1;
	}
	long total = 0;
	for (int i = 0; i < vi.nlines; i++) {
		size_t l = ustrlen(vi.lines[i]);
		sys_write((int)fd, vi.lines[i], l);
		sys_write((int)fd, "\n", 1);
		total += (long)l + 1;
	}
	sys_close((int)fd);
	vi.dirty = 0;
	/* Format a status: "saved N bytes". */
	char *p = vi.status;
	const char *s = "saved ";
	while (*s) *p++ = *s++;
	p = udec(p, total);
	*p++ = ' '; *p++ = 'b'; *p++ = 'y'; *p++ = 't'; *p++ = 'e'; *p++ = 's';
	*p = '\0';
	return 0;
}

/* Adjust top so the cursor is visible on screen. */
static void vi_scroll_into_view(void)
{
	if (vi.cy < vi.top) vi.top = vi.cy;
	if (vi.cy >= vi.top + VI_ROWS) vi.top = vi.cy - VI_ROWS + 1;
}

static void vi_draw(void)
{
	vt_clear_screen();
	for (int r = 0; r < VI_ROWS; r++) {
		int li = vi.top + r;
		vt_move(r + 1, 1);
		if (li < vi.nlines) {
			cwrite(vi.lines[li]);
		} else {
			cputc('~');	/* vi's empty-buffer marker */
		}
		vt_clear_eol();
	}
	/* Status bar at row 24 in reverse video. */
	vt_move(24, 1);
	vt_invert_on();
	const char *m = vi.mode == VI_NORMAL  ? "-- NORMAL --"
		      : vi.mode == VI_INSERT  ? "-- INSERT --"
		      :                         "-- COMMAND --";
	cwrite(m);
	cputc(' ');
	cwrite(vi.path);
	if (vi.dirty) cwrite(" [+]");
	/* line:col on the right side -- approximate at row 24 */
	cputc(' ');
	cputc(' ');
	cprint_long(vi.cy + 1);
	cputc(':');
	cprint_long(vi.cx + 1);
	if (vi.status[0]) { cputc(' '); cputc(' '); cwrite(vi.status); }
	vt_clear_eol();
	vt_invert_off();
	if (vi.mode == VI_COMMAND) {
		vt_move(24, VI_COLS - vi.cmdlen - 2);
		cputc(':');
		for (int i = 0; i < vi.cmdlen; i++) cputc(vi.cmdbuf[i]);
	} else {
		/* Position the on-screen cursor over the buffer cursor.
		 * Clamp x to the line length to avoid wandering past
		 * trailing whitespace. */
		int len = (int)ustrlen(vi.lines[vi.cy]);
		int cx  = vi.cx > len ? len : vi.cx;
		vt_move((vi.cy - vi.top) + 1, cx + 1);
	}
}

/* ---- key handling ---- */

static void vi_clamp_cx(void)
{
	int len = (int)ustrlen(vi.lines[vi.cy]);
	if (vi.cx > len) vi.cx = len;
	if (vi.cx < 0)   vi.cx = 0;
}

static void vi_move_left(void)  { if (vi.cx > 0) vi.cx--; }
static void vi_move_right(void) { vi.cx++; vi_clamp_cx(); }
static void vi_move_up(void)
{
	if (vi.cy > 0) { vi.cy--; vi_clamp_cx(); }
}
static void vi_move_down(void)
{
	if (vi.cy + 1 < vi.nlines) { vi.cy++; vi_clamp_cx(); }
}

static void vi_insert_char(char c)
{
	char *ln = vi.lines[vi.cy];
	int len = (int)ustrlen(ln);
	if (len + 1 >= VI_MAX_LINE) return;
	for (int i = len + 1; i > vi.cx; i--) ln[i] = ln[i - 1];
	ln[vi.cx] = c;
	vi.cx++;
	vi.dirty = 1;
}

static void vi_backspace(void)
{
	if (vi.cx == 0) return;
	char *ln = vi.lines[vi.cy];
	int len = (int)ustrlen(ln);
	for (int i = vi.cx - 1; i < len; i++) ln[i] = ln[i + 1];
	vi.cx--;
	vi.dirty = 1;
}

static void vi_delete_char(void)
{
	char *ln = vi.lines[vi.cy];
	int len = (int)ustrlen(ln);
	if (vi.cx >= len) return;
	for (int i = vi.cx; i < len; i++) ln[i] = ln[i + 1];
	vi.dirty = 1;
}

static void vi_delete_line(void)
{
	if (vi.nlines == 1) { vi.lines[0][0] = '\0'; vi.cx = 0; vi.dirty = 1; return; }
	for (int i = vi.cy; i < vi.nlines - 1; i++)
		vi_copy_str(vi.lines[i], vi.lines[i + 1], VI_MAX_LINE);
	vi.nlines--;
	if (vi.cy >= vi.nlines) vi.cy = vi.nlines - 1;
	vi_clamp_cx();
	vi.dirty = 1;
}

static void vi_open_line_below(void)
{
	if (vi.nlines >= VI_MAX_LINES) return;
	for (int i = vi.nlines; i > vi.cy + 1; i--)
		vi_copy_str(vi.lines[i], vi.lines[i - 1], VI_MAX_LINE);
	vi.nlines++;
	vi.cy++;
	vi.lines[vi.cy][0] = '\0';
	vi.cx = 0;
	vi.dirty = 1;
	vi.mode = VI_INSERT;
}

static void vi_open_line_above(void)
{
	if (vi.nlines >= VI_MAX_LINES) return;
	for (int i = vi.nlines; i > vi.cy; i--)
		vi_copy_str(vi.lines[i], vi.lines[i - 1], VI_MAX_LINE);
	vi.nlines++;
	vi.lines[vi.cy][0] = '\0';
	vi.cx = 0;
	vi.dirty = 1;
	vi.mode = VI_INSERT;
}

/* Returns 1 to quit ("q" / "q!" / "wq" / "x"), 0 to stay. */
static int vi_run_command(const char *cmd)
{
	if (cmd[0] == '\0') return 0;
	if (!ustrcmp(cmd, "w"))   { vi_save(); return 0; }
	if (!ustrcmp(cmd, "wq") || !ustrcmp(cmd, "x")) { vi_save(); return 1; }
	if (!ustrcmp(cmd, "q!"))  { return 1; }
	if (!ustrcmp(cmd, "q")) {
		if (vi.dirty) {
			vi_copy_str(vi.status,
				    "unsaved changes (use q!)",
				    sizeof(vi.status));
			return 0;
		}
		return 1;
	}
	vi_copy_str(vi.status, "?", sizeof(vi.status));
	return 0;
}

static int vi_normal_key(int c, int prev_g)
{
	vi.status[0] = '\0';	/* clear stale status on next key */
	switch (c) {
	case 'h': vi_move_left(); break;
	case 'l': vi_move_right(); break;
	case 'k': vi_move_up(); break;
	case 'j': vi_move_down(); break;
	case '0': vi.cx = 0; break;
	case '$':
		vi.cx = (int)ustrlen(vi.lines[vi.cy]);
		if (vi.cx > 0) vi.cx--;
		break;
	case 'G': vi.cy = vi.nlines - 1; vi_clamp_cx(); break;
	case 'g':
		if (prev_g) { vi.cy = 0; vi_clamp_cx(); }
		return 'g';	/* tell caller we saw the first g */
	case 'i': vi.mode = VI_INSERT; break;
	case 'a': if (vi.cx < (int)ustrlen(vi.lines[vi.cy])) vi.cx++;
		  vi.mode = VI_INSERT; break;
	case 'I': vi.cx = 0; vi.mode = VI_INSERT; break;
	case 'A': vi.cx = (int)ustrlen(vi.lines[vi.cy]);
		  vi.mode = VI_INSERT; break;
	case 'o': vi_open_line_below(); break;
	case 'O': vi_open_line_above(); break;
	case 'x': vi_delete_char(); break;
	case 'd':
		if (prev_g == 'd') vi_delete_line();
		else               return 'd';
		break;
	case ':':
		vi.mode = VI_COMMAND;
		vi.cmdlen = 0;
		break;
	default: break;
	}
	return 0;
}

static void cmd_vi(int argc, char *argv[])
{
	if (argc < 2) { cwrite("usage: vi <path>\r\n"); return; }
	char path[128];
	resolve_path(argv[1], path, sizeof(path));
	if (vi_load(path) < 0) { cwrite("vi: load failed\r\n"); return; }

	struct sigaction saved_sigint;
	editor_suspend_sigint(&saved_sigint);

	int prev_g = 0;
	int in_esc = 0, in_csi = 0;
	for (;;) {
		vi_scroll_into_view();
		vi_draw();

		int c = read_one();
		if (c < 0) { sys_yield(); continue; }

		if (in_csi) {
			in_csi = 0;
			/* Arrow keys translate to hjkl */
			if (vi.mode != VI_INSERT) {
				if      (c == 'A') vi_move_up();
				else if (c == 'B') vi_move_down();
				else if (c == 'C') vi_move_right();
				else if (c == 'D') vi_move_left();
			}
			prev_g = 0;
			continue;
		}
		if (in_esc) {
			in_esc = 0;
			if (c == '[') { in_csi = 1; continue; }
			/*
			 * Lone ESC: the prior byte was the user asking to
			 * return to NORMAL.  Switch mode -- then FALL THROUGH
			 * so c is dispatched as a fresh keypress.  Without
			 * the fall-through (older code did `continue;` here)
			 * the byte after ESC was silently swallowed -- so
			 * ESC':' looked like a no-op instead of entering
			 * COMMAND mode.
			 */
			vi.mode = VI_NORMAL;
			vi.cmdlen = 0;
			prev_g = 0;
			/* fall through to the dispatch below */
		}
		if (c == 0x1B) {	/* ESC */
			if (vi.mode == VI_COMMAND) {
				vi.mode = VI_NORMAL;
				vi.cmdlen = 0;
				continue;
			}
			in_esc = 1;
			continue;
		}

		if (vi.mode == VI_INSERT) {
			if (c == '\r' || c == '\n') {
				/* Split the line at cx. */
				if (vi.nlines >= VI_MAX_LINES) continue;
				char *ln = vi.lines[vi.cy];
				char tail[VI_MAX_LINE];
				vi_copy_str(tail, ln + vi.cx, sizeof(tail));
				ln[vi.cx] = '\0';
				for (int i = vi.nlines; i > vi.cy + 1; i--)
					vi_copy_str(vi.lines[i],
						    vi.lines[i - 1],
						    VI_MAX_LINE);
				vi.nlines++;
				vi.cy++;
				vi_copy_str(vi.lines[vi.cy], tail, VI_MAX_LINE);
				vi.cx = 0;
				vi.dirty = 1;
				continue;
			}
			if (c == 0x08 || c == 0x7F) {
				vi_backspace();
				continue;
			}
			if (c >= 0x20 && c < 0x7F) vi_insert_char((char)c);
			continue;
		}

		if (vi.mode == VI_COMMAND) {
			if (c == '\r' || c == '\n') {
				vi.cmdbuf[vi.cmdlen] = '\0';
				int quit = vi_run_command(vi.cmdbuf);
				vi.cmdlen = 0;
				vi.mode = VI_NORMAL;
				if (quit) break;
				continue;
			}
			if (c == 0x08 || c == 0x7F) {
				if (vi.cmdlen > 0) vi.cmdlen--;
				continue;
			}
			if (c >= 0x20 && c < 0x7F
			    && vi.cmdlen + 1 < (int)sizeof(vi.cmdbuf)) {
				vi.cmdbuf[vi.cmdlen++] = (char)c;
			}
			continue;
		}

		/* NORMAL mode */
		prev_g = vi_normal_key(c, prev_g);
	}
	/* Restore the terminal -- clear and put the cursor below. */
	vt_clear_screen();
	vt_move(1, 1);

	editor_restore_sigint(&saved_sigint);
}

/*
 * Spawnable worker.  cmd_spawn hands sys_spawn a function pointer to
 * this; the kernel sets the new thread up at EL0 with arg in x0.
 *
 * We can't legally call cwrite here without locking -- two user
 * threads writing into /dev/console at the same time interleave at
 * the byte level.  For a learning demo that's a feature, not a bug:
 * the worker's lines wind up interspersed with the shell's, which
 * proves they're real concurrent threads.
 */
__attribute__((used))
static void worker_main(long arg)
{
	/* Loop "forever" -- enough iterations that an interactive
	 * `kill <tid>` has time to land.  Each sys_yield is an SVC,
	 * so check_signals on its way back means a pending fatal
	 * signal terminates the worker within one tick. */
	for (int i = 0; i < 100000; i++) {
		if ((i % 20000) == 0) {
			char buf[80];
			const char *p = "worker: tid=";
			char *q = buf;
			while (*p) *q++ = *p++;
			q = udec(q, sys_getpid());
			const char *p2 = " arg=";
			while (*p2) *q++ = *p2++;
			q = udec(q, arg);
			const char *p3 = " iter=";
			while (*p3) *q++ = *p3++;
			udec(q, i);
			sys_log(buf);
		}
		sys_yield();
	}
	sys_exit();
}

static void cmd_spawn(int argc, char *argv[])
{
	long a = (argc > 1) ? uparse_long(argv[1]) : 0;
	long tid = sys_spawn(worker_main, a);
	cwrite("spawn: tid=");
	cprint_long(tid);
	cwrite("\r\n");
}

/* halt -- ask QEMU to exit via ARM semihosting.  Only does anything
 * useful when QEMU was started with -semihosting-config enable=on;
 * make run-thrifty / run-gui pass that flag.  Otherwise the host
 * sees an illegal-instruction trap from EL1, which still terminates
 * the run, just less tidily. */
static void cmd_halt(int argc, char *argv[])
{
	(void)argc; (void)argv;
	cwrite("halt: requesting QEMU exit...\r\n");
	sys_halt();
}

/* exec <path> [args] -- load and run an ELF from /bin (or any path).
 * Spawns a new thread for the ELF, inherits fds, then blocks until
 * the process exits.  Like a real shell's fork+exec+wait but without
 * the fork (single address space for now). */
static void cmd_exec(int argc, char *argv[])
{
	if (argc < 2) { cwrite("usage: exec <path>\r\n"); return; }
	char path[128];
	resolve_path(argv[1], path, sizeof(path));
	long tid = sys_execve(path);
	if (tid < 0) {
		cwrite("exec: failed to load '"); cwrite(path); cwrite("'\r\n");
		return;
	}
	sys_wait((int)tid);
}

/* ftrace <on|off|reset|dump>  -- control the per-CPU function tracer.
 *
 *   on / off   flip the global recording switch
 *   reset      drop all events
 *   dump       cat /proc/ftrace (default if no arg)
 *
 * Writes the verb to /proc/ftrace; the kernel side parses it.  When
 * the kernel was built without TRACE=1 the ring is permanently empty
 * but the verbs still work (they just flip a no-op flag).
 */
static void cmd_ftrace(int argc, char *argv[])
{
	const char *verb = (argc >= 2) ? argv[1] : "dump";
	if (!ustrcmp(verb, "dump")) {
		char *catargv[2] = { "cat", "/proc/ftrace" };
		cmd_cat(2, catargv);
		return;
	}
	long fd = sys_open("/proc/ftrace", 0);
	if (fd < 0) { cwrite("ftrace: cannot open /proc/ftrace\r\n"); return; }
	long w = sys_write((int)fd, verb, ustrlen(verb));
	sys_close((int)fd);
	cwrite("ftrace: "); cwrite(verb); cwrite(" -> ");
	cprint_long(w); cwrite("\r\n");
}

/* kill <tid> [sig]  -- POSIX numbering; default is SIGTERM (15). */
static void cmd_kill(int argc, char *argv[])
{
	if (argc < 2) { cwrite("usage: kill <tid> [sig]\r\n"); return; }
	int tid = (int)uparse_long(argv[1]);
	int sig = (argc > 2) ? (int)uparse_long(argv[2]) : SIGTERM;
	long r  = sys_kill(tid, sig);
	if (r < 0) { cwrite("kill: failed\r\n"); return; }
	cwrite("kill: sig "); cprint_long(sig);
	cwrite(" -> tid "); cprint_long(tid); cwrite("\r\n");
}

/* -------- kc: Norton-Commander-style two-panel file manager --------
 *
 * Textual include keeps kc.c isolated as its own file while sharing
 * cwd / cwrite / cputc / vt_move / path_canon / resolve_path / udec
 * / ustrlen with the shell.  Defining kc_main here as a function (not
 * a separate exec) means the shell can call it without our nascent
 * spawn-and-wait model getting involved.
 */
#include "kc.c"

/* -------- dispatch -------- */

static void dispatch(char *line)
{
	char *argv[TOK_MAX];
	int argc = tokenize(line, argv, TOK_MAX);
	if (argc == 0) return;

	if      (!ustrcmp(argv[0], "help"))   cmd_help();
	else if (!ustrcmp(argv[0], "pid"))    cmd_pid();
	else if (!ustrcmp(argv[0], "pwd"))    cmd_pwd();
	else if (!ustrcmp(argv[0], "cd"))     cmd_cd(argc, argv);
	else if (!ustrcmp(argv[0], "ls"))     cmd_ls(argc, argv);
	else if (!ustrcmp(argv[0], "lsl"))    cmd_lsl(argc, argv);
	else if (!ustrcmp(argv[0], "open"))   cmd_open(argc, argv);
	else if (!ustrcmp(argv[0], "close"))  cmd_close();
	else if (!ustrcmp(argv[0], "read"))   cmd_read(argc, argv);
	else if (!ustrcmp(argv[0], "write"))  cmd_write(argc, argv);
	else if (!ustrcmp(argv[0], "push"))   cmd_push(argc, argv);
	else if (!ustrcmp(argv[0], "pop"))    cmd_pop();
	else if (!ustrcmp(argv[0], "cat"))    cmd_cat(argc, argv);
	else if (!ustrcmp(argv[0], "echo"))   cmd_echo(argc, argv);
	else if (!ustrcmp(argv[0], "ked"))    cmd_ked(argc, argv);
	else if (!ustrcmp(argv[0], "vi"))     cmd_vi(argc, argv);
	else if (!ustrcmp(argv[0], "kc"))     cmd_kc(argc, argv);
	else if (!ustrcmp(argv[0], "touch"))  cmd_touch(argc, argv);
	else if (!ustrcmp(argv[0], "mkdir"))  cmd_mkdir(argc, argv);
	else if (!ustrcmp(argv[0], "rm"))     cmd_rm(argc, argv);
	else if (!ustrcmp(argv[0], "rmdir"))  cmd_rmdir(argc, argv);
	else if (!ustrcmp(argv[0], "append")) cmd_append(argc, argv);
	else if (!ustrcmp(argv[0], "spawn"))  cmd_spawn(argc, argv);
	else if (!ustrcmp(argv[0], "kill"))   cmd_kill(argc, argv);
	else if (!ustrcmp(argv[0], "exec"))   cmd_exec(argc, argv);
	else if (!ustrcmp(argv[0], "halt"))   cmd_halt(argc, argv);
	else if (!ustrcmp(argv[0], "ftrace")) cmd_ftrace(argc, argv);
	else {
		/* Try /usr/bin/<argv[0]> */
		char path[128];
		const char *prefix = "/usr/bin/";
		size_t i = 0;
		while (prefix[i] && i + 1 < sizeof(path)) { path[i] = prefix[i]; i++; }
		const char *n = argv[0];
		while (*n && i + 1 < sizeof(path)) path[i++] = *n++;
		path[i] = '\0';
		long tid = sys_execve(path);
		if (tid < 0) {
			cwrite(argv[0]); cwrite(": command not found\r\n");
		} else {
			sys_wait((int)tid);
		}
	}
}

/* -------- entry -------- */

__attribute__((noreturn, section(".text._start")))
void _start(void)
{
	{
		char buf[80];
		const char *p = "init: pid=";
		char *q = buf;
		while (*p) *q++ = *p++;
		udec(q, sys_getpid());
		sys_log(buf);
	}

	/*
	 * Probe the user-pointer validation.  sys_log on a kernel
	 * address used to leak kernel memory; now it returns -1 and
	 * kprintfs a rejection.
	 */
	{
		long good = sys_log("init: uaccess probe (good user pointer)");
		long bad  = sys_log((const char *)0x80000); /* kernel text */
		char buf[80];
		const char *p = "init: uaccess probe good=";
		char *q = buf;
		while (*p) *q++ = *p++;
		q = udec(q, good);
		const char *p2 = " bad=";
		while (*p2) *q++ = *p2++;
		udec(q, bad);
		sys_log(buf);
	}

	fd_console = (int)sys_open("/dev/console", 0); /* fd 0 = stdin  */
	if (fd_console < 0) {
		sys_log("init: open /dev/console failed");
		for (;;) sys_yield();
	}
	sys_open("/dev/console", 0); /* fd 1 = stdout (for exec'd programs) */
	sys_open("/dev/console", 0); /* fd 2 = stderr */

	/*
	 * Catch Ctrl-C.  The console driver intercepts byte 0x03,
	 * SIGINTs whichever thread is currently parked on its read
	 * wait queue (that's us, blocked in sys_read).  Without a
	 * handler installed here, SIGINT's default action is to
	 * terminate -- which would kill the shell on the first ^C.
	 */
	{
		struct sigaction sa;
		sa.sa_handler = sigint_handler;
		sa.sa_mask    = 0;
		sa.sa_flags   = 0;
		sys_sigaction(SIGINT, &sa, 0);
	}

	cwrite("\r\nkappara shell (userspace) -- type 'help' for commands\r\n");

	char line[LINE_MAX];
	for (;;) {
		prompt();
		read_line(line, sizeof(line));
		dispatch(line);
	}
}

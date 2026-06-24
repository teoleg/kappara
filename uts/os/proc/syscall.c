/*
 * kernel/syscall.c -- syscall table + dispatcher
 * ==============================================
 *
 * What this file is
 * -----------------
 * The portable middle of the syscall path.  Architecture-specific
 * trap dispatchers (arch/aarch64/trap.c, arch/arm/trap.c) decode
 * their saved trap frame, pull the syscall number and arguments
 * out of the right registers, and call syscall_dispatch().  This
 * file owns the table that maps numbers to handler functions and
 * the handlers themselves.
 *
 * Layout
 * ------
 *
 *     user / kernel       arch trap entry             syscall_dispatch
 *     ----------------    ----------------------      -----------------
 *     mov  x8/r7, #SYS_*  vectors.S saves frame   --> table[num](args)
 *     mov  args            trap_dispatch picks                |
 *     svc  #0              the SVC case and pulls             v
 *                          (num, a0..a5) out of frame    handler returns
 *                          calls syscall_dispatch              |
 *                          stashes return value     <-- result back to a0
 *                          KERNEL_EXIT / rfeia
 *
 * What the handlers can assume
 * ----------------------------
 *   * IRQs are masked on entry (the exception entry sets DAIF on
 *     AArch64, CPSR.I on ARMv7).  Handlers that want to be
 *     preemptible can re-enable them; today none do.
 *   * Pointer args point to KERNEL memory.  Once EL0/USR exists,
 *     this contract changes: pointer args will be USER addresses
 *     and the handlers will go through copy_from_user / copy_to_user.
 *   * Returning -1 signals an error to the caller.  Once we have an
 *     errno-style channel that will be more interesting.
 *
 * Handlers in this file
 * ---------------------
 *   sys_log     write a string to the kernel console
 *   sys_getpid  return current thread's tid
 *   sys_yield   call kthread_yield (cooperative reschedule)
 */

#include <stdint.h>

#include "kappara/core/printk.h"
#include "kappara/core/string.h"
#include "kappara/proc/sched.h"
#include "kappara/proc/signal.h"
#include "kappara/proc/process.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/abi/syscall.h"
#include "kappara/core/uaccess.h"
#include "kappara/proc/user.h"
#include "kappara/fs/vfs.h"

typedef long (*syscall_fn)(long, long, long, long, long, long);

/* Resolve a syscall path argument against the calling process's cwd.
 *
 *  - Absolute paths (start with '/') copy through unchanged.
 *  - "." and ".." get the cwd prepended; vfs_lookup doesn't understand
 *    them itself, but path_canon (here) collapses them.
 *  - Relative paths get "cwd/" + path with one collapse pass.
 *
 * `path` is a kernel-side string (caller has already copy_from_user'd
 * any user pointer).  `out` is a kernel buffer of `cap` bytes.  Returns
 * `out` on success, or NULL if the result wouldn't fit. */
const char *resolve_path_kva(const char *path, char *out, size_t cap)
{
	if (!path || !out || cap < 2) return NULL;

	if (path[0] == '/') {
		size_t i = 0;
		while (path[i] && i + 1 < cap) { out[i] = path[i]; i++; }
		out[i] = '\0';
	} else {
		struct kthread *t = curthread;
		const char *cwd = "/";
		if (t && t->t_proc && t->t_proc->vm &&
		    t->t_proc->vm->cwd[0] == '/')
			cwd = t->t_proc->vm->cwd;
		size_t i = 0;
		while (cwd[i] && i + 1 < cap) { out[i] = cwd[i]; i++; }
		/* "/foo" join if cwd != "/" */
		if (i > 1 && i + 1 < cap) out[i++] = '/';
		size_t j = 0;
		while (path[j] && i + 1 < cap) out[i++] = path[j++];
		out[i] = '\0';
	}

	/* In-place collapse of "/." -> "/" and "//" -> "/".  "/.." is
	 * not handled (uncommon for the path-taking syscalls). */
	char *w = out;
	for (char *r = out; *r; r++) {
		if (r[0] == '/' && r[1] == '.' && (r[2] == '/' || r[2] == '\0')) {
			*w++ = '/';	/* keep the slash; for-loop r++ skips the dot */
			r++;
			continue;
		}
		if (r[0] == '/' && r[1] == '/') continue;
		*w++ = *r;
	}
	*w = '\0';
	/* Trim trailing slash (unless path is just "/") */
	if (w > out + 1 && w[-1] == '/') *--w = '\0';
	return out;
}

static long sys_log(long arg0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

	char kbuf[256];
	const char *msg;
	if (syscall_from_user) {
		if (strncpy_from_user(kbuf,
				      (const char *)(uintptr_t)arg0,
				      sizeof(kbuf)) < 0) {
			kprintf("sys_log: rejected user pointer %p\n",
				(void *)(uintptr_t)arg0);
			return -1;
		}
		msg = kbuf;
	} else {
		msg = (const char *)(uintptr_t)arg0;
		if (!msg)
			return -1;
	}
	kprintf("sys_log: %s\n", msg);
	return 0;
}

static long sys_getpid(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	struct kthread *t = curthread;
	return t ? (long)t->tid : 0;
}

static long sys_yield(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	/* Drain any deferred STREAMS work first; modules whose putp
	 * just queues + qenables will see their srvp called here. */
	streams_run();
	kthread_yield();
	return 0;
}

static long sys_open(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a2; (void)a3; (void)a4; (void)a5;
	return (long)sys_open_impl((const char *)(uintptr_t)a0, (int)a1);
}

static long sys_mkdir(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return (long)sys_mkdir_impl((const char *)(uintptr_t)a0);
}

static long sys_spawn(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a2; (void)a3; (void)a4; (void)a5;
	return sys_spawn_impl((uint64_t)a0, (uint64_t)a1);
}

static long sys_exit(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	sys_exit_impl((int)a0);
	return 0;	/* unreachable */
}

static long sys_unlink(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return (long)sys_unlink_impl((const char *)(uintptr_t)a0);
}

static long sys_rmdir(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return (long)sys_rmdir_impl((const char *)(uintptr_t)a0);
}

static long sys_kill(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a2; (void)a3; (void)a4; (void)a5;
	return (long)sys_kill_impl((int)a0, (int)a1);
}

/*
 * sys_halt -- ARM semihosting SYS_EXIT.
 *
 * On QEMU run with `-semihosting-config enable=on,target=native`, an
 * `hlt #0xf000` trap at EL1 with x0=0x18 (SYS_EXIT) makes QEMU exit
 * cleanly.  Lets the user kill QEMU from inside the shell when the
 * stdio Ctrl-C path isn't available (terminal multiplexer ate it).
 *
 * If semihosting isn't enabled on the host QEMU, the HLT instruction
 * generates a normal SIGILL / trap from EL1 that hits trap_dispatch
 * and panics -- which still ends the run, just less tidily.
 */
__attribute__((noreturn))
static long sys_halt(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	kprintf("halt: requesting QEMU exit via semihosting\n");
	register long x0 __asm__("x0") = 0x18;      /* SYS_EXIT          */
	register long x1 __asm__("x1") = 0x20026;   /* ApplicationExit   */
	__asm__ volatile ("hlt #0xf000"
			  : : "r"(x0), "r"(x1) : "memory");
	/* Should not return.  If semihosting is disabled the HLT
	 * triggers an exception; either way we stop here. */
	for (;;) __asm__ volatile ("wfi");
}

static long sys_ll(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a3; (void)a4; (void)a5;
	char kpath[128];
	const char *path = (const char *)(uintptr_t)a0;
	char *out = (char *)(uintptr_t)a1;
	size_t cap = (size_t)a2;

	if (syscall_from_user) {
		if (a0 == 0) {
			path = "/";
		} else if (strncpy_from_user(kpath, path, sizeof(kpath)) < 0) {
			kprintf("sys_lsl: rejected user path pointer\n");
			return -1;
		} else {
			path = kpath;
		}
		if (!user_ptr_ok(out, cap)) {
			kprintf("sys_lsl: rejected user out buf\n");
			return -1;
		}
	} else if (!path) {
		path = "/";
	}

	struct dentry *d = vfs_lookup(path);
	if (!d) {
		kprintf("sys_lsl: ENOENT '%s'\n", path);
		return -1;
	}
	return vfs_listdir_long(d, out, cap);
}

static long sys_close(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return (long)sys_close_impl((int)a0);
}

static long sys_read(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a3; (void)a4; (void)a5;
	return sys_read_impl((int)a0, (void *)(uintptr_t)a1, (size_t)a2);
}

static long sys_write(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a3; (void)a4; (void)a5;
	return sys_write_impl((int)a0, (const void *)(uintptr_t)a1, (size_t)a2);
}

static long sys_ioctl(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a3; (void)a4; (void)a5;
	return sys_ioctl_impl((int)a0, (int)a1, a2);
}

static long sys_putmsg(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a4; (void)a5;
	return sys_putmsg_impl((int)a0,
			       (const struct strbuf *)(uintptr_t)a1,
			       (const struct strbuf *)(uintptr_t)a2,
			       (int)a3);
}

static long sys_getmsg(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a4; (void)a5;
	return sys_getmsg_impl((int)a0,
			       (struct strbuf *)(uintptr_t)a1,
			       (struct strbuf *)(uintptr_t)a2,
			       (int *)(uintptr_t)a3);
}

static long sys_creat(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return (long)sys_creat_impl((const char *)(uintptr_t)a0);
}

static long sys_seek(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a3; (void)a4; (void)a5;
	return sys_seek_impl((int)a0, (long)a1, (int)a2);
}

static long sys_pipe(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

	int kfds[2];
	long r = sys_pipe_impl(kfds);
	if (r < 0)
		return r;

	int *udst = (int *)(uintptr_t)a0;
	if (syscall_from_user) {
		if (copy_to_user(udst, kfds, sizeof(kfds)) < 0)
			return -1;
	} else {
		udst[0] = kfds[0];
		udst[1] = kfds[1];
	}
	return 0;
}

static long sys_ls(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a3; (void)a4; (void)a5;
	char kpath[128];
	const char *path = (const char *)(uintptr_t)a0;
	char *out = (char *)(uintptr_t)a1;
	size_t cap = (size_t)a2;

	if (syscall_from_user) {
		if (a0 == 0) {
			path = "/";
		} else if (strncpy_from_user(kpath, path, sizeof(kpath)) < 0) {
			kprintf("sys_ls: rejected user path pointer\n");
			return -1;
		} else {
			path = kpath;
		}
		if (!user_ptr_ok(out, cap)) {
			kprintf("sys_ls: rejected user out buf\n");
			return -1;
		}
	} else if (!path) {
		path = "/";
	}

	/* Resolve "." / "foo" / "./foo" against the process's cwd. */
	char resolved[128];
	const char *p = resolve_path_kva(path, resolved, sizeof(resolved));
	if (!p) return -1;
	struct dentry *d = vfs_lookup(p);
	if (!d) return -1;	/* ENOENT — normal control flow, no kprintf */
	return vfs_listdir(d, out, cap);
}

/* SYS_chdir(const char *path) -- update vm_map->cwd. */
static long sys_chdir(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	struct kthread *t = curthread;
	if (!t || !t->t_proc || !t->t_proc->vm) return -1;

	char kpath[128];
	const char *path = (const char *)(uintptr_t)a0;
	if (syscall_from_user) {
		if (strncpy_from_user(kpath, path, sizeof(kpath)) < 0)
			return -1;
		path = kpath;
	}

	char resolved[128];
	const char *p = resolve_path_kva(path, resolved, sizeof(resolved));
	if (!p) return -1;
	struct dentry *d = vfs_lookup(p);
	if (!d || !d->d_inode || d->d_inode->i_type != INODE_DIR) return -1;

	struct vm_map *vm = t->t_proc->vm;
	size_t i = 0;
	while (p[i] && i + 1 < sizeof(vm->cwd)) { vm->cwd[i] = p[i]; i++; }
	vm->cwd[i] = '\0';
	return 0;
}

/* SYS_getcwd(char *buf, size_t cap) -- copy vm_map->cwd to user. */
static long sys_getcwd(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a2; (void)a3; (void)a4; (void)a5;
	struct kthread *t = curthread;
	if (!t || !t->t_proc || !t->t_proc->vm) return -1;
	char *user_buf = (char *)(uintptr_t)a0;
	size_t cap = (size_t)a1;
	struct vm_map *vm = t->t_proc->vm;
	const char *src = (vm->cwd[0] == '/') ? vm->cwd : "/";

	size_t n = 0;
	while (src[n] && n + 1 < cap) n++;
	if (syscall_from_user) {
		if (copy_to_user(user_buf, src, n) < 0) return -1;
		char nul = '\0';
		if (copy_to_user(user_buf + n, &nul, 1) < 0) return -1;
	} else {
		for (size_t i = 0; i < n; i++) user_buf[i] = src[i];
		user_buf[n] = '\0';
	}
	return (long)n;
}

static long sys_sigaction(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a3; (void)a4; (void)a5;
	return sys_sigaction_impl((int)a0,
				  (const struct sigaction_k *)(uintptr_t)a1,
				  (struct sigaction_k *)(uintptr_t)a2);
}

static long sys_sigprocmask(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a3; (void)a4; (void)a5;
	return sys_sigprocmask_impl((int)a0,
				    (const uint32_t *)(uintptr_t)a1,
				    (uint32_t *)(uintptr_t)a2);
}

static long sys_sigsuspend(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return sys_sigsuspend_impl((uint32_t)a0);
}

static long sys_wait(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return sys_wait_impl((int)a0);
}

#define EXEC_MAX_ARGS    32
#define EXEC_MAX_ARGLEN  128

static long sys_execve(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a2; (void)a3; (void)a4; (void)a5;
	char kpath[128];
	const char *path = (const char *)(uintptr_t)a0;
	const char **uargv = (const char **)(uintptr_t)a1;

	if (syscall_from_user) {
		if (strncpy_from_user(kpath, path, sizeof(kpath)) < 0)
			return -1;
		path = kpath;
	}

	/* Copy argv strings from user space into static kernel buffers.
	 * Static storage avoids blowing the kernel stack with 4 KB of
	 * argv data; only one exec runs at a time (shell waits). */
	static char kargv_buf[EXEC_MAX_ARGS * EXEC_MAX_ARGLEN];
	static const char *kargv[EXEC_MAX_ARGS + 1];
	int kargc = 0;
	char *kbuf = kargv_buf;

	if (uargv) {
		while (kargc < EXEC_MAX_ARGS) {
			const char *uarg = NULL;
			/* read one argv[kargc] pointer from user space */
			if (copy_from_user(&uarg, uargv + kargc, sizeof(uarg)) < 0)
				break;
			if (!uarg) break;
			long n = strncpy_from_user(kbuf, uarg, EXEC_MAX_ARGLEN);
			if (n < 0) break;
			kargv[kargc++] = kbuf;
			kbuf += (size_t)n + 1;
		}
	}
	kargv[kargc] = NULL;

	return sys_execve_impl(path, kargc, kargv);
}

/*
 * SYS_sigreturn is intentionally NOT in this table.  It needs to
 * mutate the trap frame in place, so arch/aarch64/trap.c special-
 * cases it before reaching the generic dispatch path.  Putting a
 * stub here would silently shadow the real handler.
 */

static long sys_setpgid(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a2; (void)a3; (void)a4; (void)a5;
	return sys_setpgid_impl((int)a0, (int)a1);
}
static long sys_getpgrp(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return sys_getpgrp_impl();
}
static long sys_setsid(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return sys_setsid_impl();
}
static long sys_tcsetpgrp(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a2; (void)a3; (void)a4; (void)a5;
	return sys_tcsetpgrp_impl((int)a0, (int)a1);
}
static long sys_tcgetpgrp(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return sys_tcgetpgrp_impl((int)a0);
}

static long sys_brk(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return sys_brk_impl((uint64_t)(unsigned long)a0);
}

/*
 * SYS_clock_gettime -- monotonic CNTPCT-derived timespec.
 *
 * The clk_id argument is ignored: we have one clock (the AArch64
 * generic timer, monotonic since boot).  Reading CNTPCT_EL0 +
 * CNTFRQ_EL0 from EL1 is always safe.
 *
 * The user buffer is { tv_sec; tv_nsec } -- two longs, matching
 * lib/libc/include/time.h.  copy_to_user gates the write so a bad
 * pointer returns -1 instead of panicking the kernel.
 */
struct __ts_kernel { long tv_sec; long tv_nsec; };

static long sys_clock_gettime(long a0, long a1, long a2,
			      long a3, long a4, long a5)
{
	(void)a0; (void)a2; (void)a3; (void)a4; (void)a5;
	uint64_t cnt, freq;
	__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(cnt));
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
	if (freq == 0)
		return -1;
	struct __ts_kernel kts;
	kts.tv_sec  = (long)(cnt / freq);
	uint64_t rem = cnt - (uint64_t)kts.tv_sec * freq;
	kts.tv_nsec = (long)((rem * 1000000000ULL) / freq);
	void *uts = (void *)(uintptr_t)a1;
	if (syscall_from_user) {
		if (copy_to_user(uts, &kts, sizeof(kts)) < 0)
			return -1;
	} else {
		*(struct __ts_kernel *)uts = kts;
	}
	return 0;
}

/* DYNAMIC.md stage 7: dlopen/dlsym dispatch.  Both wrappers pull the
 * path (or symbol name) string from userland via strncpy_from_user and
 * hand off to sys_dlopen_impl / sys_dlsym_impl in user.c. */
static long sys_dlopen(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	char kpath[128];
	const char *path = (const char *)(uintptr_t)a0;
	if (syscall_from_user) {
		if (strncpy_from_user(kpath, path, sizeof(kpath)) < 0)
			return 0;
		path = kpath;
	}
	return (long)sys_dlopen_impl(path);
}

static long sys_dlsym(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a2; (void)a3; (void)a4; (void)a5;
	char kname[64];
	const char *name = (const char *)(uintptr_t)a1;
	if (syscall_from_user) {
		if (strncpy_from_user(kname, name, sizeof(kname)) < 0)
			return 0;
		name = kname;
	}
	return (long)sys_dlsym_impl((uint64_t)a0, name);
}

static long sys_dlclose(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return sys_dlclose_impl((uint64_t)a0);
}

static long sys_dlerror(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a2; (void)a3; (void)a4; (void)a5;
	/* sys_dlerror_impl handles copy_to_user via syscall_from_user. */
	return sys_dlerror_impl((char *)(uintptr_t)a0, (unsigned)a1);
}

static const syscall_fn syscall_table[SYS_MAX] = {
	[SYS_log]    = sys_log,
	[SYS_getpid] = sys_getpid,
	[SYS_yield]  = sys_yield,
	[SYS_open]   = sys_open,
	[SYS_close]  = sys_close,
	[SYS_read]   = sys_read,
	[SYS_write]  = sys_write,
	[SYS_ioctl]  = sys_ioctl,
	[SYS_putmsg] = sys_putmsg,
	[SYS_getmsg] = sys_getmsg,
	[SYS_ls]     = sys_ls,
	[SYS_pipe]   = sys_pipe,
	[SYS_creat]  = sys_creat,
	[SYS_seek]   = sys_seek,
	[SYS_mkdir]  = sys_mkdir,
	[SYS_spawn]  = sys_spawn,
	[SYS_exit]   = sys_exit,
	[SYS_unlink] = sys_unlink,
	[SYS_rmdir]  = sys_rmdir,
	[SYS_kill]   = sys_kill,
	[SYS_ll]          = sys_ll,
	[SYS_halt]        = sys_halt,
	[SYS_sigaction]   = sys_sigaction,
	[SYS_sigprocmask] = sys_sigprocmask,
	[SYS_sigsuspend]  = sys_sigsuspend,
	[SYS_wait]        = sys_wait,
	[SYS_execve]      = sys_execve,
	[SYS_setpgid]     = sys_setpgid,
	[SYS_getpgrp]     = sys_getpgrp,
	[SYS_setsid]      = sys_setsid,
	[SYS_tcsetpgrp]   = sys_tcsetpgrp,
	[SYS_tcgetpgrp]   = sys_tcgetpgrp,
	[SYS_brk]         = sys_brk,
	[SYS_clock_gettime] = sys_clock_gettime,
	[SYS_dlopen]      = sys_dlopen,
	[SYS_dlsym]       = sys_dlsym,
	[SYS_dlclose]     = sys_dlclose,
	[SYS_dlerror]     = sys_dlerror,
	[SYS_chdir]       = sys_chdir,
	[SYS_getcwd]      = sys_getcwd,
};

/* INDIE.md Path B: Linux syscall translation.
 *
 * Maps Linux aarch64 syscall numbers (asm-generic/unistd.h) to our
 * native handlers when the calling binary uses the Linux ABI.  Today
 * we route any `x8 >= 50` through the Linux table -- our native
 * syscalls all sit below 42 and the Linux numbers we care about for
 * static-musl programs (write=64, exit=93, brk=214, mmap=222,
 * mprotect=226, exit_group=94, writev=66) all start at 50+.
 *
 * The handful of overlapping low numbers (Linux ioctl=29 vs SYS_getpgrp)
 * stays on the native table for now; binaries using those won't work
 * until a real per-process ABI flag lands. */
#define LINUX_NR_MAX 280

static long linux_sys_write(long a0, long a1, long a2, long a3, long a4, long a5)
{
	return syscall_table[SYS_write](a0, a1, a2, a3, a4, a5);
}
static long linux_sys_read(long a0, long a1, long a2, long a3, long a4, long a5)
{
	return syscall_table[SYS_read](a0, a1, a2, a3, a4, a5);
}
static long linux_sys_close(long a0, long a1, long a2, long a3, long a4, long a5)
{
	return syscall_table[SYS_close](a0, a1, a2, a3, a4, a5);
}
static long linux_sys_exit(long a0, long a1, long a2, long a3, long a4, long a5)
{
	return syscall_table[SYS_exit](a0, a1, a2, a3, a4, a5);
}
static long linux_sys_brk(long a0, long a1, long a2, long a3, long a4, long a5)
{
	return syscall_table[SYS_brk](a0, a1, a2, a3, a4, a5);
}
static long linux_sys_getpid(long a0, long a1, long a2, long a3, long a4, long a5)
{
	return syscall_table[SYS_getpid](a0, a1, a2, a3, a4, a5);
}
static long linux_sys_set_tid_address(long a0, long a1, long a2, long a3, long a4, long a5)
{
	/* Linux set_tid_address(int *tid_addr): musl uses it for thread
	 * cancellation.  We have no concept; pretend we set it and return
	 * the current tid so musl is happy. */
	(void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return syscall_table[SYS_getpid](0, 0, 0, 0, 0, 0);
}
static long linux_sys_mprotect(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a3; (void)a4; (void)a5;
	return sys_mprotect_impl((uint64_t)a0, (uint64_t)a1, (int)a2);
}

static long linux_sys_mmap(long a0, long a1, long a2, long a3, long a4, long a5)
{
	return sys_mmap_impl((uint64_t)a0, (uint64_t)a1, (int)a2,
			      (int)a3, (int)a4, (uint64_t)a5);
}

static long linux_sys_munmap(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a2; (void)a3; (void)a4; (void)a5;
	return sys_munmap_impl((uint64_t)a0, (uint64_t)a1);
}
static long linux_sys_ioctl(long a0, long a1, long a2, long a3, long a4, long a5)
{
	return syscall_table[SYS_ioctl](a0, a1, a2, a3, a4, a5);
}
static long linux_sys_writev(long a0, long a1, long a2, long a3, long a4, long a5)
{
	/* writev(fd, iovec[], niov).  iovec = {void *base; size_t len;}.
	 * Pasted-together by walking the iovec and issuing SYS_write per
	 * entry.  Not atomic, but musl's startup printf only uses 1-elt
	 * iovecs in practice. */
	(void)a3; (void)a4; (void)a5;
	int fd = (int)a0;
	const long *iov = (const long *)(uintptr_t)a1;
	int niov = (int)a2;
	if (!iov || niov <= 0) return 0;
	long total = 0;
	for (int i = 0; i < niov; i++) {
		long base = iov[i * 2];
		long len  = iov[i * 2 + 1];
		if (len == 0) continue;
		long w = syscall_table[SYS_write](fd, base, len, 0, 0, 0);
		if (w < 0) return w;
		total += w;
		if (w < len) break;
	}
	return total;
}

static const syscall_fn linux_syscall_table[LINUX_NR_MAX] = {
	[29]  = linux_sys_ioctl,
	[57]  = linux_sys_close,
	[63]  = linux_sys_read,
	[64]  = linux_sys_write,
	[66]  = linux_sys_writev,
	[93]  = linux_sys_exit,
	[94]  = linux_sys_exit,	/* exit_group */
	[96]  = linux_sys_set_tid_address,
	[172] = linux_sys_getpid,
	[214] = linux_sys_brk,
	[215] = linux_sys_munmap,
	[222] = linux_sys_mmap,
	[226] = linux_sys_mprotect,
};

long syscall_dispatch(long num, long a0, long a1, long a2,
		      long a3, long a4, long a5)
{
	/* Linux-ABI dispatch first: any number >= 50 with a Linux entry
	 * routes through the translation table. */
	if ((unsigned long)num >= 50 && (unsigned long)num < LINUX_NR_MAX &&
	    linux_syscall_table[num]) {
		return linux_syscall_table[num](a0, a1, a2, a3, a4, a5);
	}
	if ((unsigned long)num >= SYS_MAX || !syscall_table[num]) {
		kprintf("syscall: bad number %ld\n", num);
		return -1;
	}
	return syscall_table[num](a0, a1, a2, a3, a4, a5);
}

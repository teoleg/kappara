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
#include "kappara/proc/sched.h"
#include "kappara/proc/signal.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/abi/syscall.h"
#include "kappara/core/uaccess.h"
#include "kappara/proc/user.h"
#include "kappara/fs/vfs.h"

typedef long (*syscall_fn)(long, long, long, long, long, long);

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

	struct dentry *d = vfs_lookup(path);
	if (!d) return -1;	/* ENOENT — normal control flow, no kprintf */
	return vfs_listdir(d, out, cap);
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
};

long syscall_dispatch(long num, long a0, long a1, long a2,
		      long a3, long a4, long a5)
{
	if ((unsigned long)num >= SYS_MAX || !syscall_table[num]) {
		kprintf("syscall: bad number %ld\n", num);
		return -1;
	}
	return syscall_table[num](a0, a1, a2, a3, a4, a5);
}

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

#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"
#include "kappara/syscall.h"

typedef long (*syscall_fn)(long, long, long, long, long, long);

static long sys_log(long arg0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	const char *msg = (const char *)(uintptr_t)arg0;
	if (!msg)
		return -1;
	kprintf("sys_log: %s\n", msg);
	return 0;
}

static long sys_getpid(long a0, long a1, long a2, long a3, long a4, long a5)
{
	(void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	extern struct kthread *cur;
	return cur ? (long)cur->tid : 0;
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
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
	return (long)sys_open_impl((const char *)(uintptr_t)a0);
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

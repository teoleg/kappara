/*
 * kernel/main.c -- AArch64 kmain orchestration
 * ============================================
 *
 * The "top-level" of the kernel after boot.S has done its part.
 * Brings each subsystem up in dependency order, then drops into a
 * round-robin yield loop with the timer driving preemption.
 *
 *   uart_init    -- earliest, so subsequent kprintf() works
 *   trap_init    -- install VBAR_EL1 so faults dump instead of looping
 *   mmu_init     -- identity-map the address space, turn on caches
 *   pmm_init     -- enrol the free RAM range with the page allocator
 *   kmem_init    -- prepare the size-caches for kmalloc()
 *   sched_init   -- make this code path tid 0 ("main")
 *   timer_init   -- arm the generic timer at 100 Hz
 *   create demo threads
 *   msr daifclr, #2  -- finally unmask IRQs; the timer now preempts us
 *
 * Two demo threads (alpha and beta) and kmain itself loop printing
 * a counter with a busy spin in between.  Because the spin takes
 * roughly 1 ms and the tick is 10 ms, each thread runs ~7 iterations
 * per slice and the output cycles main / alpha / beta / main / ...
 */

#include <stdint.h>

#include "kappara/kmem.h"
#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"
#include "kappara/string.h"
#include "kappara/syscall.h"
#include "kappara/vfs.h"
#include "kappara/timer.h"
#include "kappara/trap.h"
#include "kappara/uart.h"
#include "kappara/user.h"

extern char __kernel_end[];

/* Pi 3 / BCM2837: usable RAM ends here; above is the peripheral window. */
#define AARCH64_RAM_END	0x3F000000UL

static unsigned current_el(void)
{
	unsigned long el;
	__asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
	return (unsigned)((el >> 2) & 3u);
}

/*
 * The original kernel-mode SVC demo and the alpha/beta busy-printer
 * threads were dropped in favour of the interactive ksh shell.
 * Everything they exercised (svc dispatch, fd table, /dev/loop, I_PUSH,
 * delay-srvp, putmsg/getmsg) is now reachable by typing commands at
 * the kappara# prompt.
 */
#if 0
static void syscall_demo(void)
{
	do_syscall(SYS_log, (long)(uintptr_t)"hello from kernel-svc!", 0, 0);
	long pid = do_syscall(SYS_getpid, 0, 0, 0);
	kprintf("syscall: getpid -> %ld\n", pid);

	/* sys_ls("/dev") via syscall -- prove path lookup works through SVC. */
	char lsbuf[128];
	long lsn = do_syscall(SYS_ls,
			      (long)(uintptr_t)"/dev",
			      (long)(uintptr_t)lsbuf,
			      sizeof(lsbuf));
	kprintf("syscall: ls /dev (%ld bytes):\n", lsn);
	for (long i = 0; i < lsn; i++)
		uart_putc(lsbuf[i]);

	/* Non-existent path: sys_open should fail cleanly. */
	long fmiss = do_syscall(SYS_open, (long)(uintptr_t)"/dev/nope", 0, 0);
	kprintf("syscall: open(\"/dev/nope\") -> %ld (expect -1)\n", fmiss);

	/* STREAMS-over-syscalls: open the "/dev/loop" driver, write a payload,
	 * read it back.  Then I_PUSH "upper" and watch the same payload
	 * come back uppercased.  Then I_POP and confirm lower-case again. */
	long fd = do_syscall(SYS_open, (long)(uintptr_t)"/dev/loop", 0, 0);
	kprintf("syscall: open(\"/dev/loop\") -> %ld\n", fd);

	char buf[32];
	const char msg1[] = "hello stream";
	do_syscall(SYS_write, fd, (long)(uintptr_t)msg1, sizeof(msg1) - 1);
	long n = do_syscall(SYS_read, fd, (long)(uintptr_t)buf, sizeof(buf));
	print_buf("syscall: read (loop only)", buf, n);

	long r = do_syscall(SYS_ioctl, fd, I_PUSH, (long)(uintptr_t)"upper");
	kprintf("syscall: ioctl(I_PUSH, \"upper\") -> %ld\n", r);

	const char msg2[] = "now in upper case please";
	do_syscall(SYS_write, fd, (long)(uintptr_t)msg2, sizeof(msg2) - 1);
	n = do_syscall(SYS_read, fd, (long)(uintptr_t)buf, sizeof(buf));
	print_buf("syscall: read (after I_PUSH upper)", buf, n);

	r = do_syscall(SYS_ioctl, fd, I_POP, 0);
	kprintf("syscall: ioctl(I_POP) -> %ld\n", r);

	const char msg3[] = "back to plain";
	do_syscall(SYS_write, fd, (long)(uintptr_t)msg3, sizeof(msg3) - 1);
	n = do_syscall(SYS_read, fd, (long)(uintptr_t)buf, sizeof(buf));
	print_buf("syscall: read (after I_POP)", buf, n);

	do_syscall(SYS_close, fd, 0, 0);
	kprintf("syscall: close(%ld) done\n", fd);

	/* ----- delay module + service procedure demo --------------------- */
	kprintf("\n--- delay module (service-proc deferred I/O) ---\n");
	fd = do_syscall(SYS_open, (long)(uintptr_t)"/dev/loop", 0, 0);
	do_syscall(SYS_ioctl, fd, I_PUSH, (long)(uintptr_t)"delay");

	const char dmsg[] = "via delay";
	do_syscall(SYS_write, fd, (long)(uintptr_t)dmsg, sizeof(dmsg) - 1);

	n = do_syscall(SYS_read, fd, (long)(uintptr_t)buf, sizeof(buf));
	kprintf("syscall: immediate read = %ld bytes (queued in delay)\n", n);

	/* Two yields: drain delay_wq, then delay_rq. */
	do_syscall(SYS_yield, 0, 0, 0);
	do_syscall(SYS_yield, 0, 0, 0);

	n = do_syscall(SYS_read, fd, (long)(uintptr_t)buf, sizeof(buf));
	print_buf("syscall: read after two yields", buf, n);
	do_syscall(SYS_close, fd, 0, 0);

	/* ----- putmsg / getmsg demo ------------------------------------- */
	kprintf("\n--- putmsg / getmsg (M_PROTO + M_DATA) ---\n");
	fd = do_syscall(SYS_open, (long)(uintptr_t)"/dev/loop", 0, 0);

	const char ctlbytes[]  = "ctl-hdr";
	const char databytes[] = "payload bytes here";
	struct strbuf pctl  = { .maxlen = 0,
				.len    = (int)sizeof(ctlbytes) - 1,
				.buf    = (void *)ctlbytes };
	struct strbuf pdata = { .maxlen = 0,
				.len    = (int)sizeof(databytes) - 1,
				.buf    = (void *)databytes };
	long r2 = do_syscall_4(SYS_putmsg, fd,
			       (long)(uintptr_t)&pctl,
			       (long)(uintptr_t)&pdata, 0);
	kprintf("syscall: putmsg -> %ld\n", r2);

	char gctlbuf[16], gdatabuf[32];
	struct strbuf gctl  = { .maxlen = sizeof(gctlbuf),  .len = 0,
				.buf = gctlbuf  };
	struct strbuf gdata = { .maxlen = sizeof(gdatabuf), .len = 0,
				.buf = gdatabuf };
	int gflags = 0;
	r2 = do_syscall_4(SYS_getmsg, fd,
			  (long)(uintptr_t)&gctl,
			  (long)(uintptr_t)&gdata,
			  (long)(uintptr_t)&gflags);
	kprintf("syscall: getmsg -> %ld (flags=%d)\n", r2, gflags);
	print_buf("  ctl ", gctlbuf,  gctl.len);
	print_buf("  data", gdatabuf, gdata.len);

	do_syscall(SYS_close, fd, 0, 0);
}
#endif

void kmain(void)
{
	uart_init();
	trap_init();

	kprintf("\nkappara: hello from aarch64\n");
	kprintf("        no soup for you, only streams\n");
	kprintf("        running at EL%u\n", current_el());

	mmu_init();
	pmm_init((uintptr_t)__kernel_end, AARCH64_RAM_END);
	kmem_init();

	vfs_init();
	streams_head_init();
	user_init();

	kprintf("\n");
	vfs_dump_tree(vfs_root());

	sched_init();
	timer_init(100);

	/* Console RX feeder: polls PL011 RX FIFO and putnext's received
	 * bytes into /dev/console's read chain.  Spawned before ksh so
	 * the feeder thread is in the ready queue when ksh issues its
	 * first SYS_read. */
	kthread_create("uart_rx", uart_rx_main, NULL);

	/* Spawn the user-mode init process; once scheduled it eret's
	 * into EL0 at user/init.c's _start, opens /dev/console, and
	 * runs the shell entirely in userspace from there on. */
	user_spawn();

	__asm__ volatile ("msr daifclr, #2");

	/* Main becomes the idle thread: yield forever so the timer/ksh
	 * get all the CPU they want. */
	for (;;) {
		kthread_yield();
		__asm__ volatile ("wfi");
	}
}

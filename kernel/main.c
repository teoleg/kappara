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
#include "kappara/streams.h"
#include "kappara/string.h"
#include "kappara/syscall.h"
#include "kappara/timer.h"
#include "kappara/trap.h"
#include "kappara/uart.h"

extern char __kernel_end[];

/* Pi 3 / BCM2837: usable RAM ends here; above is the peripheral window. */
#define AARCH64_RAM_END	0x3F000000UL

static unsigned current_el(void)
{
	unsigned long el;
	__asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
	return (unsigned)((el >> 2) & 3u);
}

static void spin(unsigned long n)
{
	for (volatile unsigned long i = 0; i < n; i++)
		;
}

static void demo(void *arg)
{
	const char *name = arg;
	for (unsigned long n = 0; ; n++) {
		kprintf("[%s] n=%lu\n", name, n);
		spin(500000);
	}
}

/*
 * STREAMS smoke test.  Build a paired queue (rq/wq) backed by a
 * trivial put procedure that simply enqueues messages on its own
 * queue.  Send a single M_DATA message down the write side, then
 * pull it back off via getq and verify the bytes survive.
 */
static int demo_putp(queue_t *q, mblk_t *mp)
{
	putq(q, mp);
	return 0;
}

static struct module_info demo_minfo = {
	.mi_idnum  = 42,
	.mi_idname = "demo",
	.mi_minpsz = 0,
	.mi_maxpsz = 256,
	.mi_hiwat  = 4096,
	.mi_lowat  = 1024,
};

static struct qinit demo_rinit = {
	.qi_putp = demo_putp, .qi_minfo = &demo_minfo
};

static struct qinit demo_winit = {
	.qi_putp = demo_putp, .qi_minfo = &demo_minfo
};

static void streams_demo(void)
{
	streams_init();

	queue_t *rq = kmalloc(sizeof(*rq));
	queue_t *wq = kmalloc(sizeof(*wq));
	queue_init_pair(rq, wq, &demo_rinit, &demo_winit);

	static const char payload[] = "no soup for you, only streams";
	const size_t plen = sizeof(payload) - 1;

	mblk_t *mp = allocb(plen, 0);
	kmemcpy(mp->b_wptr, payload, plen);
	mp->b_wptr += plen;

	kprintf("streams: msg built, dsize=%lu  (db_ref=%d type=0x%x)\n",
		(unsigned long)msgdsize(mp),
		mp->b_datap->db_ref, mp->b_datap->db_type);

	wq->q_qinfo->qi_putp(wq, mp);
	kprintf("streams: after putp, q_count=%lu q_flag=0x%x\n",
		(unsigned long)wq->q_count, wq->q_flag);

	mblk_t *got = getq(wq);
	if (!got) {
		kprintf("streams: getq returned NULL?\n");
		return;
	}
	kprintf("streams: dequeued (%lu bytes): ",
		(unsigned long)msgdsize(got));
	for (unsigned char *p = got->b_rptr; p < got->b_wptr; p++)
		uart_putc((char)*p);
	uart_putc('\n');

	freemsg(got);
	kfree(rq);
	kfree(wq);
	kprintf("streams: demo complete\n");
}

/*
 * Syscall demo (AArch64 calling convention).  Issue `svc #0` with
 * x8 = syscall number and x0..x5 = args; trap_dispatch picks up
 * ESR_EL1.EC == 0x15, routes to syscall_dispatch, and the return
 * value is stuffed back into x0 before eret.
 */
static long do_syscall(long num, long a0, long a1, long a2)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x8 __asm__("x8") = num;
	__asm__ volatile (
		"svc	#0\n"
		: "+r"(x0)
		: "r"(x1), "r"(x2), "r"(x8)
		: "memory", "cc");
	return x0;
}

static void syscall_demo(void)
{
	do_syscall(SYS_log, (long)(uintptr_t)"hello from kernel-svc!", 0, 0);
	long pid = do_syscall(SYS_getpid, 0, 0, 0);
	kprintf("syscall: getpid -> %ld\n", pid);
}

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

	streams_demo();

	sched_init();
	syscall_demo();

	timer_init(100);

	kthread_create("alpha", demo, (void *)"alpha");
	kthread_create("beta",  demo, (void *)"beta");

	__asm__ volatile ("msr daifclr, #2");
	kprintf("[main] IRQs unmasked; entering scheduler loop\n");

	for (unsigned long n = 0; ; n++) {
		kprintf("[main] n=%lu\n", n);
		spin(500000);
	}
}

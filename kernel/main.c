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
#include "kappara/timer.h"
#include "kappara/trap.h"
#include "kappara/uart.h"

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

void kmain(void)
{
	uart_init();
	trap_init();

	kprintf("\nkappara: hello from aarch64\n");
	kprintf("        no soup for you, only streams\n");
	kprintf("        running at EL%u\n", current_el());

	mmu_init();
	pmm_init();
	kmem_init();
	sched_init();
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

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

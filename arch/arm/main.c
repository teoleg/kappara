/*
 * arch/arm/main.c -- ARMv7-A kmain
 * ================================
 *
 * Mirrors the AArch64 kmain end-to-end: console, vectors, MMU, pmm,
 * kmem, sched, generic timer + GICv2.  After the timer is armed and
 * cpsie i unmasks interrupts, three threads (main / alpha / beta)
 * round-robin under timer preemption.
 *
 *   uart_init       PL011 console at 0x09000000
 *   trap_init       VBAR-rooted vector table
 *   mmu_init        LPAE identity map + caches on
 *   pmm_init        free-list over [__kernel_end .. 0x50000000)
 *   kmem_init       size-cache slab heap
 *   sched_init      register kmain itself as tid 0
 *   timer_init      100 Hz generic-timer tick via GIC
 *   cpsie i         unmask CPSR.I; the IRQ vector now drives sched_tick
 */

#include <stdint.h>

#include "kappara/kmem.h"
#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/syscall.h"
#include "kappara/timer.h"
#include "kappara/trap.h"
#include "kappara/uart.h"

extern char __kernel_end[];

/* QEMU virt cortex-a15 with -m 256: RAM at 0x40000000..0x50000000. */
#define ARM_RAM_END	0x50000000UL

static unsigned arm_current_mode(void)
{
	uint32_t cpsr;
	__asm__ volatile ("mrs %0, cpsr" : "=r"(cpsr));
	return (unsigned)(cpsr & 0x1F);
}

static const char *mode_name(unsigned m)
{
	switch (m) {
	case 0x10: return "USR";
	case 0x11: return "FIQ";
	case 0x12: return "IRQ";
	case 0x13: return "SVC";
	case 0x17: return "ABT";
	case 0x1A: return "HYP";
	case 0x1B: return "UND";
	case 0x1F: return "SYS";
	default:   return "??";
	}
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

/* ARMv7 EABI syscall: number in r7, args r0..r5, return r0. */
static long do_syscall(long num, long a0, long a1, long a2)
{
	register long r0 __asm__("r0") = a0;
	register long r1 __asm__("r1") = a1;
	register long r2 __asm__("r2") = a2;
	register long r7 __asm__("r7") = num;
	__asm__ volatile (
		"svc	#0\n"
		: "+r"(r0)
		: "r"(r1), "r"(r2), "r"(r7)
		: "memory", "cc");
	return r0;
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

	kprintf("\nkappara: hello from armv7\n");
	kprintf("        no soup for you, only streams\n");
	kprintf("        running in %s mode (cpsr.M=0x%x)\n",
		mode_name(arm_current_mode()), arm_current_mode());

	mmu_init();
	pmm_init((uintptr_t)__kernel_end, ARM_RAM_END);
	kmem_init();
	sched_init();
	syscall_demo();
	timer_init(100);

	kthread_create("alpha", demo, (void *)"alpha");
	kthread_create("beta",  demo, (void *)"beta");

	__asm__ volatile ("cpsie i" ::: "memory");
	kprintf("[main] IRQs unmasked; entering scheduler loop\n");

	for (unsigned long n = 0; ; n++) {
		kprintf("[main] n=%lu\n", n);
		spin(500000);
	}
}

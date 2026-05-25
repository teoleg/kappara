/*
 * arch/arm/main.c -- ARMv7-A kmain
 * ================================
 *
 * Brings up the same subsystems as kernel/main.c on AArch64, but
 * without preemption: the timer + GIC + IRQ unwind aren't here yet,
 * so threads cooperate via explicit kthread_yield() calls.
 *
 * Order of bring-up (mirrors the AArch64 path):
 *   uart_init       PL011 console
 *   trap_init       VBAR-rooted vector table
 *   mmu_init        LPAE identity map + caches on
 *   pmm_init        free-list over [__kernel_end .. 0x80000000)
 *   kmem_init       size-cache slab heap
 *   sched_init      register kmain itself as tid 0
 *
 * Two demo threads (alpha, beta) each loop a few times calling
 * kthread_yield(); main does the same.  Expected output rotates
 * main / alpha / beta / main / alpha / beta / ...
 */

#include <stdint.h>

#include "kappara/kmem.h"
#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/trap.h"
#include "kappara/uart.h"

extern char __kernel_end[];

/* QEMU virt cortex-a15 with -m 256 has RAM at 0x40000000..0x50000000. */
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

static void demo(void *arg)
{
	const char *name = arg;
	for (int i = 0; i < 5; i++) {
		kprintf("[%s] i=%d\n", name, i);
		kthread_yield();
	}
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

	kthread_create("alpha", demo, (void *)"alpha");
	kthread_create("beta",  demo, (void *)"beta");

	for (int i = 0; i < 5; i++) {
		kprintf("[main] i=%d\n", i);
		kthread_yield();
	}

	kpanic("end of armv7 kmain");
}

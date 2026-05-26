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
#include "kappara/ksh.h"
#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/stream_head.h"
#include "kappara/syscall.h"
#include "kappara/timer.h"
#include "kappara/trap.h"
#include "kappara/uart.h"
#include "kappara/vfs.h"

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
	vfs_init();
	streams_head_init();

	kprintf("\n");
	vfs_dump_tree(vfs_root());

	sched_init();
	timer_init(100);

	kthread_create("uart_rx", uart_rx_main, NULL);
	kthread_create("ksh",     ksh_main,     NULL);

	__asm__ volatile ("cpsie i" ::: "memory");

	for (;;) {
		kthread_yield();
		__asm__ volatile ("wfi");
	}
}

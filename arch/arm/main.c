/*
 * arch/arm/main.c -- ARMv7-A early kmain
 * ======================================
 *
 * The "top-level" of the kernel for the ARMv7 build, equivalent in
 * scope to kernel/main.c on AArch64 -- with a smaller set of
 * subsystems wired in so we can grow the port one piece at a time.
 *
 * Currently brings up:
 *   uart_init   PL011 console at 0x09000000
 *   trap_init   VBAR-rooted vector table
 *   mmu_init    LPAE identity map + caches on
 *
 * Demonstrates the MMU is live by writing a sentinel to a RAM
 * address through the MMU and reading it back, then panics so the
 * boot returns control to the QEMU monitor.
 */

#include <stdint.h>

#include "kappara/mmu.h"
#include "kappara/printk.h"
#include "kappara/trap.h"
#include "kappara/uart.h"

extern char __kernel_end[];

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

	/* MMU sanity check: write/read a sentinel just past the kernel. */
	volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)__kernel_end;
	p[0] = 0xdeadbeefu;
	p[1] = 0xcafef00du;
	kprintf("mmu-test: wrote/read %p -> 0x%lx 0x%lx (kernel_end=%p)\n",
		(void *)p, (unsigned long)p[0], (unsigned long)p[1],
		(void *)__kernel_end);

	kpanic("end of armv7 kmain");
}

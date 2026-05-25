/*
 * arch/arm/main.c -- ARMv7-A early kmain
 * ======================================
 *
 * Minimal kmain for the ARMv7 build.  Equivalent in scope to the early
 * AArch64 kmain in kernel/main.c after step 2: console up, vectors
 * installed, then a deliberate exception so we can see the trap dumper
 * decode the frame.  When MMU + scheduler land on ARMv7 this will be
 * superseded by the portable kernel/main.c, but for now it lives in
 * arch/arm/ so the AArch64 kmain (which uses arch-specific MMU/IRQ
 * APIs not yet present on ARMv7) stays unaffected.
 *
 * What it does
 * ------------
 *   1. Bring up the PL011 UART (arch/arm/uart.c).
 *   2. Install arm_vectors via VBAR (arch/arm/trap.c).
 *   3. Print the current processor mode -- expect SVC after boot.S has
 *      done the HYP->SVC drop.
 *   4. Execute UDF #0 (permanently undefined ARM instruction) to fire
 *      the "undef" vector and force the trap dumper to print.
 *
 * Expected output (on QEMU -M virt -cpu cortex-a15):
 *
 *   kappara: hello from armv7
 *           running in SVC mode (cpsr.M=0x13)
 *           VBAR installed; firing udf #0...
 *
 *   !! trap: undef (vec=1)
 *      pc=0x4001xxxx  spsr=0x600000d3 (pre-mode=SVC)
 *      r0 =...  r1 =...
 *      ...
 *   panic: unhandled trap
 */

#include <stdint.h>

#include "kappara/printk.h"
#include "kappara/trap.h"
#include "kappara/uart.h"

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
	case 0x16: return "MON";
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
	kprintf("        VBAR installed; firing udf #0...\n");

	__asm__ volatile ("udf #0");

	kpanic("returned from udf?");
}

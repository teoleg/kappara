/*
 * uts/virt/main.c -- minimal kmain for the QEMU virt target
 *
 * Phase 1 of the virt port.  Initialises the PL011 UART, prints a
 * splash, and halts.  Proves the build chain (ARCH=virt) wires
 * together: boot.S transitions EL2->EL1, linker.ld places everything
 * at 0x40080000, the shared uart.c works against PLAT_PL011_BASE for
 * virt's 0x09000000.
 *
 * Phase 2 will bring in the rest of the kernel (sched, vfs, streams,
 * IP, TCP) with stubs for the Pi-specific drivers virt doesn't have
 * (framebuffer, mailbox, mini-UART, BCM2836 IPI).  Phase 3 adds the
 * virtio-mmio probe + virtio-net driver, and Phase 4 wires telnet on
 * top so external hosts can connect.
 */

#include "kappara/arch/uart.h"

void kmain(void)
{
	uart_init();
	uart_puts_panic("\nkappara hello from QEMU virt (aarch64)\n");
	uart_puts_panic("phase 1: bring-up halt -- shell coming in phase 2\n");
	for (;;)
		__asm__ volatile ("wfi");
}

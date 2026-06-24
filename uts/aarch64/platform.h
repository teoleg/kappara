/*
 * arch/aarch64/platform.h -- per-board MMIO map + clocks
 *
 * Selects a platform-specific header at compile time.  Today the only
 * supported platform is QEMU `virt`, which is also our path to AWS
 * EC2 Graviton (UEFI/ACPI shape, GIC v3, PCIe ECAM -- see docs/AWS.md).
 *
 * The PLATFORM_VIRT define is set by the Makefile.  Pi-family
 * platform headers (pi3.h, pi4.h) and their drivers live in
 * attic/raspi3b/ -- kept for history but no longer built.  Restoring
 * them would mean copying the headers back and re-adding the
 * raspi3b ARCH branch to the Makefile.
 *
 * Required symbols (platform/<name>.h must define)
 * ------------------------------------------------
 *   PLAT_NAME                  short string for printk
 *   PLAT_PERIPH_BASE           start of the SoC peripheral window (VA == PA)
 *   PLAT_PERIPH_END            one-past-end of the peripheral window
 *   PLAT_LOCAL_PERIPH_BASE     GIC base / ARM-local peripheral block
 *   PLAT_RAM_END               first byte beyond usable RAM (= where
 *                              pmm stops registering pages)
 *   PLAT_PL011_BASE            primary PL011 UART MMIO base
 *   PLAT_TIMER_CONTROL         per-core generic-timer routing reg
 *   PLAT_TIMER_IRQ_SOURCE      per-core IRQ source reg
 *   PLAT_TIMER_CNTPNSIRQ_BIT   bit position of the non-secure phys
 *                              timer's IRQ in the routing/source regs
 */

#ifndef KAPPARA_AARCH64_PLATFORM_H
#define KAPPARA_AARCH64_PLATFORM_H

#include "platform/virt.h"

#endif

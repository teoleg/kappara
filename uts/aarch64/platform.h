/*
 * arch/aarch64/platform.h -- per-board MMIO map + clocks
 *
 * Selects a platform-specific header at compile time and exposes the
 * shared symbols every Pi-family target uses.  Today the only
 * implementation is platform/pi3.h; platform/pi4.h slots in next to
 * it when we add Pi 4 support.
 *
 * Selection
 * ---------
 * Default = PLATFORM_PI3 (matches our existing QEMU raspi3b target).
 * Override with `make ARCH=aarch64 PLATFORM=pi4` once pi4.h exists.
 *
 * Required symbols (each platform/<name>.h must define)
 * -----------------------------------------------------
 *   PLAT_NAME                  short string for printk
 *   PLAT_PERIPH_BASE           start of the SoC peripheral window (VA == PA)
 *   PLAT_PERIPH_END            one-past-end of the peripheral window
 *   PLAT_LOCAL_PERIPH_BASE     ARM-local / GIC base (the 1 GB block
 *                              above peripherals; used to be BCM2836
 *                              local on Pi 3, becomes GIC-400 on Pi 4)
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

#if defined(PLATFORM_PI4)
#  include "platform/pi4.h"
#else
#  include "platform/pi3.h"
#endif

#endif

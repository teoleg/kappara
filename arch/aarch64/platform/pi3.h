/*
 * arch/aarch64/platform/pi3.h -- Raspberry Pi 3 (BCM2837) board layout
 *
 * Memory and peripheral map matched by:
 *   - real Pi 3 Model A+ / B / B+ / Zero 2 W
 *   - QEMU `qemu-system-aarch64 -M raspi3b`
 *
 * Peripheral window
 * -----------------
 *   0x3F000000 .. 0x40000000     BCM2837 peripherals (1 GB block,
 *                                  but only the first ~256 KB are
 *                                  populated -- PL011 UART, mailbox,
 *                                  GPIO, system timer, ...)
 *   0x40000000 .. 0x80000000     ARM-local peripherals (BCM2836)
 *                                  per-core timer routing,
 *                                  mailboxes, per-core IRQ source.
 *
 * UART
 * ----
 *   PL011 at 0x3F201000.  Input clock is 48 MHz on real hardware
 *   (provided enable_uart=1 + core_freq=250 in config.txt); QEMU
 *   ignores baud divisors entirely.  IBRD=26, FBRD=3 yields 115200.
 *
 * Timer routing
 * -------------
 *   The BCM2836 ARM-local block at 0x40000000 routes the per-core
 *   generic-timer IRQs to each core's IRQ/FIQ lines.  We use CNTPNSIRQ
 *   (bit 1) -- the non-secure physical timer -- on core 0.
 *
 * RAM
 * ---
 *   Up to 1 GB on Pi 3 B/B+; the peripheral window starts at
 *   0x3F000000 so that's where pmm stops registering pages.
 */

#ifndef KAPPARA_PLATFORM_PI3_H
#define KAPPARA_PLATFORM_PI3_H

#define PLAT_NAME			"raspi3b (BCM2837)"

/* Physical memory layout. */
#define PLAT_PERIPH_BASE		0x3F000000UL
#define PLAT_PERIPH_END			0x40000000UL
#define PLAT_LOCAL_PERIPH_BASE		0x40000000UL
#define PLAT_RAM_END			PLAT_PERIPH_BASE

/* PL011 UART. */
#define PLAT_PL011_BASE			(PLAT_PERIPH_BASE + 0x00201000UL)

/* BCM2836 per-core timer routing (offsets from PLAT_LOCAL_PERIPH_BASE). */
#define PLAT_TIMER_CONTROL		(PLAT_LOCAL_PERIPH_BASE + 0x40UL)
#define PLAT_TIMER_IRQ_SOURCE		(PLAT_LOCAL_PERIPH_BASE + 0x60UL)
#define PLAT_TIMER_CNTPNSIRQ_BIT	(1u << 1)

#endif

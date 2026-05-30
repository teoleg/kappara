/*
 * arch/aarch64/platform/pi4.h -- Raspberry Pi 4 (BCM2711) board layout
 *
 * STUB.  Numbers are correct per the BCM2711 ARM Peripherals PDF
 * but this file is not yet exercised -- selecting PLATFORM=pi4 in
 * the Makefile will compile but boot will fail until the IRQ
 * routing (GIC-400 vs Pi 3's BCM2836 local block) is plumbed too.
 *
 * Pi 4 vs Pi 3 deltas this file expresses:
 *
 *   Peripheral base    0x3F000000   ->  0xFE000000      (+ 0xBF000000)
 *   PL011 UART         0x3F201000   ->  0xFE201000
 *   ARM-local block    0x40000000   ->  N/A (GIC-400 at 0xFF841000)
 *   RAM end            implicit 1G  ->  configurable 1/2/4/8 GB
 *
 * The Pi 4 has a real GIC-400 instead of the BCM2836 routing block,
 * so PLAT_TIMER_CONTROL / PLAT_TIMER_IRQ_SOURCE will go away when
 * the actual port lands; this stub leaves them at 0 so a misconfigured
 * build fails loudly.  Don't ship PLATFORM=pi4 until that work is done.
 */

#ifndef KAPPARA_PLATFORM_PI4_H
#define KAPPARA_PLATFORM_PI4_H

#define PLAT_NAME			"raspi4b (BCM2711) -- STUB"

#define PLAT_PERIPH_BASE		0xFE000000UL
#define PLAT_PERIPH_END			0xFF000000UL
#define PLAT_LOCAL_PERIPH_BASE		0xFF800000UL	/* GIC-400 region */
#define PLAT_RAM_END			PLAT_PERIPH_BASE

#define PLAT_PL011_BASE			(PLAT_PERIPH_BASE + 0x00201000UL)

/* These are placeholders -- Pi 4 will use the GIC-400 directly, not
 * a BCM2836-style local routing block.  Setting them to 0 means a
 * Pi-4 build hooked through timer.c without a GIC port would visibly
 * fail at MMIO time, which is the goal until the real driver lands. */
#define PLAT_TIMER_CONTROL		0UL
#define PLAT_TIMER_IRQ_SOURCE		0UL
#define PLAT_TIMER_CNTPNSIRQ_BIT	0u

#endif

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

/* VideoCore mailbox interface (channel 8 = property tags).  This is
 * how we talk to the GPU firmware to allocate framebuffers, query
 * temperatures, set clocks, etc. */
#define PLAT_MBOX_BASE			(PLAT_PERIPH_BASE + 0x0000B880UL)

/* BCM2836 per-core timer routing (offsets from PLAT_LOCAL_PERIPH_BASE).
 *
 * The BCM2836 has one timer-control + one irq-source register per core,
 * at +0x40 and +0x60 respectively, packed 4 bytes apart.  Core N's
 * registers live at +0x40+4N and +0x60+4N.
 *
 *   Core(N) Timer Control:  0x40 + 4N   (TIMERnCTL)
 *   Core(N) Mailbox  IRQs:  0x50 + 4N   (MBOXnCTL)
 *   Core(N) IRQ Source:     0x60 + 4N
 *   Core(N) FIQ Source:     0x70 + 4N
 *
 * Bit positions inside each register are uniform across all four cores;
 * we only ever set bit 1 (CNTPNSIRQ -> IRQ).
 */
#define PLAT_TIMER_CONTROL(cpu)		\
	(PLAT_LOCAL_PERIPH_BASE + 0x40UL + 4UL * (cpu))
#define PLAT_TIMER_IRQ_SOURCE(cpu)	\
	(PLAT_LOCAL_PERIPH_BASE + 0x60UL + 4UL * (cpu))
#define PLAT_TIMER_CNTPNSIRQ_BIT	(1u << 1)

/* BCM2836 inter-processor mailboxes.  Each core has four 32-bit
 * write-set / read-clear mailbox slots.  We use mailbox 0 as the
 * single-bit "wake-up IPI" reason; reasons beyond "the IRQ fired"
 * are not encoded -- the IRQ itself is what kicks WFI'd cores.
 *
 *   Core(N) MailboxN-IRQ Ctrl:    0x50 + 4N  (bit M = enable mailbox M)
 *   Core(N) Mailbox M Set:        0x80 + 0x10*N + 4M  (write to send IPI)
 *   Core(N) Mailbox M Read/Clear: 0xC0 + 0x10*N + 4M  (write-1-to-clear)
 *
 * IRQ_SOURCE bit positions for mailbox pendings: bit (4+M) for mailbox M
 * on this core.  Mailbox 0 pending therefore shows up at IRQ_SOURCE bit 4.
 */
#define PLAT_MBOX_IRQ_CTL(cpu)		\
	(PLAT_LOCAL_PERIPH_BASE + 0x50UL + 4UL * (cpu))
#define PLAT_MBOX_SET(cpu, mbox)	\
	(PLAT_LOCAL_PERIPH_BASE + 0x80UL + 0x10UL * (cpu) + 4UL * (mbox))
#define PLAT_MBOX_RDCLR(cpu, mbox)	\
	(PLAT_LOCAL_PERIPH_BASE + 0xC0UL + 0x10UL * (cpu) + 4UL * (mbox))
#define PLAT_IRQSRC_MBOX0_BIT		(1u << 4)

#endif

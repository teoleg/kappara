/*
 * arch/arm/timer.c -- ARMv7 generic timer + GICv2 IRQ routing
 * ===========================================================
 *
 * What this file is
 * -----------------
 * The ARMv7 cousin of arch/aarch64/timer.c.  Programs the ARMv7
 * generic timer for periodic preemption, programs the GICv2 (the
 * Generic Interrupt Controller) to deliver the timer IRQ to the
 * CPU, and contains the IRQ-side dispatch the vector table calls
 * into.
 *
 * Why a GIC at all (vs the BCM2836 routing block on AArch64)
 * ---------------------------------------------------------
 * The Pi 3 doesn't have a real GIC -- it has a custom ARM-local
 * peripheral instead.  Almost every other ARMv7/ARMv8 SoC, including
 * the QEMU `virt` machine and the MediaTek MT8125, uses an actual
 * GIC.  This file is the standard pattern.
 *
 * GICv2 layout (memory-mapped)
 * ----------------------------
 *
 *     0x08000000  GICD  distributor   (one per system)
 *       0x000     CTLR        enable
 *       0x080     IGROUPR     group 0 vs group 1
 *       0x100..   ISENABLER   set-enable bitmap (one bit per IRQ)
 *       0x180..   ICENABLER   clear-enable bitmap
 *       0x400..   IPRIORITYR  8-bit priority per IRQ
 *       0x800..   ITARGETSR   target CPU mask per IRQ (SPIs only)
 *       0xC00..   ICFGR       edge/level config per IRQ
 *
 *     0x08010000  GICC  CPU interface  (one per core)
 *       0x00      CTLR        enable
 *       0x04      PMR         priority mask
 *       0x08      BPR         binary point
 *       0x0C      IAR         acknowledge (read = pending IRQ INTID)
 *       0x10      EOIR        end of interrupt (write IAR value)
 *
 * INTID space
 * -----------
 *   0..15    SGI  software-generated (used for inter-CPU IPIs)
 *   16..31   PPI  private peripheral interrupts (per-CPU; this is
 *                  where generic-timer IRQs live)
 *   32..      SPI  shared peripheral interrupts (the rest)
 *
 * The ARMv7 non-secure physical timer (CNTPNSIRQ) is PPI 14 ->
 * INTID 30.  PPIs auto-target the CPU they fire on, so we don't
 * write GICD_ITARGETSR for it.
 *
 * IRQ ack / EOI pattern
 * ---------------------
 * On every IRQ:
 *   1. read IAR  -- gives the INTID that fired and "activates" it
 *                    in the GIC (prevents redelivery of same source)
 *   2. handle    -- ack the device (here: reload CNTP_TVAL_EL0
 *                    so the timer line deasserts and re-arms)
 *   3. EOI       -- write IAR back to GICC_EOIR, which drops the
 *                    priority and deactivates the IRQ
 *
 * EOI before yielding
 * -------------------
 * sched_tick may call into context_switch and never return to this
 * frame on the current thread.  If we EOI'd AFTER yielding, the GIC
 * would think the timer was still active and refuse to redeliver
 * to the new thread -- effectively disabling preemption for any
 * thread other than the one that just yielded.  So we EOI first,
 * then drive the scheduler.
 *
 * ARMv7 generic timer access (CP15 c14)
 * -------------------------------------
 *   CNTFRQ      MRC p15, 0, Rt, c14, c0, 0   (R/O)
 *   CNTP_CTL    MRC/MCR p15, 0, Rt, c14, c2, 1
 *   CNTP_TVAL   MRC/MCR p15, 0, Rt, c14, c2, 0   (relative deadline)
 *   CNTP_CVAL   MRRC/MCRR p15, 2, Rt, Rt2, c14   (absolute, 64-bit)
 */

#include <stdint.h>

#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/timer.h"

#define GICD_BASE		0x08000000UL
#define GICC_BASE		0x08010000UL

#define GICD_CTLR		(GICD_BASE + 0x000)
#define GICD_ISENABLER(n)	(GICD_BASE + 0x100 + 4 * (n))
#define GICD_IPRIORITYR(n)	(GICD_BASE + 0x400 + 4 * (n))

#define GICC_CTLR		(GICC_BASE + 0x00)
#define GICC_PMR		(GICC_BASE + 0x04)
#define GICC_BPR		(GICC_BASE + 0x08)
#define GICC_IAR		(GICC_BASE + 0x0C)
#define GICC_EOIR		(GICC_BASE + 0x10)

#define TIMER_INTID		30u	/* CNTPNSIRQ = PPI 14 = INTID 30 */

static uint64_t tick_cycles;

static inline uint32_t mmio_read(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void mmio_write(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

static inline uint32_t read_cntfrq(void)
{
	uint32_t v;
	__asm__ volatile ("mrc p15, 0, %0, c14, c0, 0" : "=r"(v));
	return v;
}

static inline void write_cntp_tval(uint32_t v)
{
	__asm__ volatile ("mcr p15, 0, %0, c14, c2, 0" :: "r"(v));
}

static inline void write_cntp_ctl(uint32_t v)
{
	__asm__ volatile ("mcr p15, 0, %0, c14, c2, 1" :: "r"(v));
}

static void gic_init(void)
{
	/* Quiesce the distributor while we configure it. */
	mmio_write(GICD_CTLR, 0);

	/* Enable the timer IRQ (INTID 30) in the distributor. */
	mmio_write(GICD_ISENABLER(TIMER_INTID / 32), 1u << (TIMER_INTID % 32));

	/* Mid-range priority (0xA0) for INTID 30 (byte index 30 % 4 = 2). */
	{
		uint32_t reg = GICD_IPRIORITYR(TIMER_INTID / 4);
		uint32_t cur = mmio_read(reg);
		unsigned shift = (TIMER_INTID % 4) * 8;
		cur = (cur & ~(0xFFu << shift)) | (0xA0u << shift);
		mmio_write(reg, cur);
	}

	/* CPU interface: accept all priorities, no preemption grouping. */
	mmio_write(GICC_PMR, 0xFFu);
	mmio_write(GICC_BPR, 0);
	mmio_write(GICC_CTLR, 1u);

	/* Now turn the distributor on. */
	mmio_write(GICD_CTLR, 1u);
}

void timer_init(unsigned hz)
{
	uint32_t freq = read_cntfrq();
	tick_cycles = freq / hz;

	gic_init();

	write_cntp_tval((uint32_t)tick_cycles);
	write_cntp_ctl(1u);

	kprintf("timer: cntfrq=%lu Hz, tick=%u Hz (%lu cycles)\n",
		(unsigned long)freq, hz, (unsigned long)tick_cycles);
}

static void timer_irq(void)
{
	write_cntp_tval((uint32_t)tick_cycles);
	sched_tick();
}

void irq_dispatch(void)
{
	uint32_t iar = mmio_read(GICC_IAR);
	uint32_t intid = iar & 0x3FFu;

	/* 1020..1023 are reserved (spurious / no IRQ); don't EOI those. */
	if (intid >= 1020u)
		return;

	/* EOI first so the GIC will redeliver to other threads if we yield. */
	mmio_write(GICC_EOIR, iar);

	if (intid == TIMER_INTID) {
		timer_irq();
	} else {
		kprintf("irq: unhandled INTID=%u\n", intid);
	}
}

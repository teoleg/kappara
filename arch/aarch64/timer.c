/*
 * arch/aarch64/timer.c -- ARMv8 generic timer + BCM2836 IRQ routing
 * =================================================================
 *
 * What this file is
 * -----------------
 * The driver that turns the ARMv8 architectural per-CPU generic
 * timer into a periodic scheduler tick.  This is the heartbeat that
 * makes the scheduler preemptive: every 1/HZ seconds the timer
 * fires CNTPNSIRQ, the IRQ vector takes us into irq_dispatch(), and
 * we call sched_tick() which yields to the next ready thread.
 *
 * Two parts:
 *   1. The CPU's generic timer (programmed via CNTP_CTL_EL0 /
 *      CNTP_TVAL_EL0).  Architectural -- same code would work on any
 *      ARMv8-A CPU.
 *   2. The BCM2836/2837-specific routing block that actually delivers
 *      CNTPNSIRQ to core 0's IRQ line.  Without this, the timer
 *      fires inside the core but no IRQ is taken.
 *
 * Why the routing block exists
 * ----------------------------
 * The BCM2836/2837 has no GIC (the Pi 4 / BCM2711 does).  Instead a
 * tiny "ARM local" peripheral at 0x40000000 routes per-core timer,
 * mailbox, and legacy-GPU interrupts to each core's IRQ / FIQ lines.
 * For our purposes we touch two registers:
 *
 *   0x40000040  TIMER_CONTROL (core 0)
 *               bit 0  CNTPSIRQ  -> IRQ
 *               bit 1  CNTPNSIRQ -> IRQ   <-- we set this
 *               bit 2  CNTHPIRQ  -> IRQ
 *               bit 3  CNTVIRQ   -> IRQ
 *               bit 4  CNTPSIRQ  -> FIQ
 *               ...    (bits 5..7 FIQ enables for the same four timers)
 *
 *   0x40000060  IRQ_SOURCE (core 0, read-only)
 *               same bit positions; reads 1 while that source is
 *               asserting an IRQ to this core.  Used by irq_dispatch
 *               to figure out who fired.
 *
 * Why CNTPNSIRQ specifically?
 * ---------------------------
 * We left HCR_EL2.RW=1 and dropped to non-secure EL1 in boot.S, so
 * the EL1 physical timer is the non-secure physical timer; its
 * out-of-band IRQ line is CNTPNSIRQ (PN = Physical Non-secure).
 *
 * The fire-and-rearm loop
 * -----------------------
 * Generic timers have two ways to set the next deadline:
 *   CNTP_CVAL_EL0  absolute counter value
 *   CNTP_TVAL_EL0  relative: writing N arms the timer for (CNTPCT + N)
 *
 * We use TVAL because relative is what "wake me every 10 ms" wants.
 * Each tick we reload TVAL with the same `tick_cycles` value; the
 * write also clears the pending condition, so the IRQ deasserts
 * until the next deadline.
 */

#include <stdint.h>

#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/timer.h"

/*
 * BCM2836/2837 "ARM local" peripherals, used by the Pi 3 to route per-core
 * timer interrupts before the generic timer hits a CPU's IRQ line.
 */
#define LOCAL_BASE		0x40000000UL
#define CORE0_TIMER_CTL		(LOCAL_BASE + 0x40)
#define CORE0_IRQ_SRC		(LOCAL_BASE + 0x60)

/* TIMER_CONTROL bits: [3:0] route CNTP{S,NS,HP,V}IRQ to IRQ, [7:4] to FIQ. */
#define ROUTE_CNTPNSIRQ_IRQ	(1u << 1)

/* IRQ_SRC bits: bit 1 = CNTPNSIRQ pending. */
#define IRQ_CNTPNSIRQ		(1u << 1)

static uint64_t tick_cycles;

static inline uint64_t read_cntfrq(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static inline void write_cntp_tval(uint64_t v)
{
	__asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void write_cntp_ctl(uint64_t v)
{
	__asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(v));
}

void timer_init(unsigned hz)
{
	uint64_t freq = read_cntfrq();
	tick_cycles = freq / hz;

	*(volatile uint32_t *)CORE0_TIMER_CTL = ROUTE_CNTPNSIRQ_IRQ;

	write_cntp_tval(tick_cycles);
	write_cntp_ctl(1);	/* ENABLE=1, IMASK=0 */

	kprintf("timer: cntfrq=%lu Hz, tick=%u Hz (%lu cycles)\n",
		(unsigned long)freq, hz, (unsigned long)tick_cycles);
}

static void timer_irq(void)
{
	write_cntp_tval(tick_cycles);
	sched_tick();
}

void irq_dispatch(void)
{
	uint32_t src = *(volatile uint32_t *)CORE0_IRQ_SRC;

	if (src & IRQ_CNTPNSIRQ) {
		timer_irq();
		return;
	}

	kprintf("irq: spurious (src=0x%x)\n", src);
}

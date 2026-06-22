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
 *      CNTPNSIRQ to each core's IRQ line.  Without this, the timer
 *      fires inside the core but no IRQ is taken.
 *
 * Why the routing block exists
 * ----------------------------
 * The BCM2836/2837 has no GIC (the Pi 4 / BCM2711 does).  Instead a
 * tiny "ARM local" peripheral at 0x40000000 routes per-core timer,
 * mailbox, and legacy-GPU interrupts to each core's IRQ / FIQ lines.
 * Each core has its OWN TIMER_CONTROL + IRQ_SOURCE register at
 *
 *   TIMER_CONTROL(N) = 0x40000040 + 4*N
 *   IRQ_SOURCE  (N) = 0x40000060 + 4*N
 *
 * with identical bit layouts:
 *
 *   bit 0  CNTPSIRQ   bit 4  mailbox 0
 *   bit 1  CNTPNSIRQ  bit 5  mailbox 1
 *   bit 2  CNTHPIRQ   bit 6  mailbox 2
 *   bit 3  CNTVIRQ    bit 7  mailbox 3
 *
 * Per-CPU bring-up
 * ----------------
 * `timer_init(hz)` runs once on core 0; it captures `tick_cycles`
 * and then calls `timer_init_this_cpu()`.  Each secondary core calls
 * `timer_init_this_cpu()` from `secondary_main` to arm its own
 * generic timer and enable the CNTPNSIRQ route for its own core.
 *
 * The generic-timer system registers (CNTP_CTL_EL0, CNTP_TVAL_EL0)
 * are per-CPU: each core's MSR/MRS lands on that core's banked
 * register.  No locking needed.
 *
 * Why CNTPNSIRQ specifically?
 * ---------------------------
 * We left HCR_EL2.RW=1 and dropped to non-secure EL1 in boot.S, so
 * the EL1 physical timer is the non-secure physical timer; its
 * out-of-band IRQ line is CNTPNSIRQ (PN = Physical Non-secure).
 *
 * The fire-and-rearm loop
 * -----------------------
 * Each tick we reload TVAL with the same `tick_cycles` value; the
 * write also clears the pending condition, so the IRQ deasserts
 * until the next deadline.
 */

#include <stdint.h>

#include "kappara/arch/ipi.h"
#include "kappara/core/printk.h"
#include "kappara/proc/sched.h"
#include "kappara/arch/timer.h"
#include "platform.h"

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

static inline unsigned this_cpu_id(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, mpidr_el1" : "=r"(v));
	return (unsigned)(v & 0xff);
}

/*
 * Arm THIS CPU's generic timer at the rate stored in `tick_cycles`
 * (set once on core 0 by timer_init) and route its CNTPNSIRQ to IRQ
 * via this CPU's TIMER_CONTROL register.  Safe to call from any core
 * after MMU is up (the local-peripheral block is in the Device-mapped
 * 1 GB at 0x40000000).
 */
void timer_init_this_cpu(void)
{
	unsigned cpu = this_cpu_id();
	*(volatile uint32_t *)PLAT_TIMER_CONTROL(cpu) = PLAT_TIMER_CNTPNSIRQ_BIT;
	write_cntp_tval(tick_cycles);
	write_cntp_ctl(1);	/* ENABLE=1, IMASK=0 */
}

void timer_init(unsigned hz)
{
	uint64_t freq = read_cntfrq();
	tick_cycles = freq / hz;

	timer_init_this_cpu();

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
	unsigned cpu = this_cpu_id();
	uint32_t src = *(volatile uint32_t *)PLAT_TIMER_IRQ_SOURCE(cpu);

	if (src & PLAT_TIMER_CNTPNSIRQ_BIT) {
		timer_irq();
		/* Fall through: mailbox might also be pending. */
	}

	if (src & PLAT_IRQSRC_MBOX0_BIT) {
		ipi_handle();
		return;
	}

	if (src & PLAT_TIMER_CNTPNSIRQ_BIT)
		return;	/* timer was the only source */

	kprintf("irq: cpu %u spurious (src=0x%x)\n", cpu, src);
}

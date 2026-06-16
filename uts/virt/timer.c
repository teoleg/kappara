/*
 * uts/virt/timer.c -- generic timer + GIC PPI 30 routing
 *
 * Same architectural generic-timer setup as the raspi3b version (the
 * MSR/MRS sequence on CNTP_CTL_EL0 / CNTP_TVAL_EL0 is unchanged across
 * any ARMv8-A CPU); the difference is how the IRQ gets delivered.
 *
 *   Pi 3   -> CNTPNSIRQ wired into the BCM2836 local-peripheral
 *             routing register at 0x40000040+4*cpu.
 *   virt   -> non-secure physical timer IRQ is PPI 30, delivered by
 *             the GIC.  Enable in GICD_ISENABLER0 (bit 30).
 *
 * The dispatch shape matches Pi: irq_dispatch() is called from the
 * vectors.S IRQ handler, decides who's pending, calls timer_irq /
 * ipi_handle as needed, EOIs the GIC.
 */

#include <stdint.h>

#include "kappara/arch/gic.h"
#include "kappara/arch/ipi.h"
#include "kappara/arch/timer.h"
#include "kappara/core/printk.h"
#include "kappara/proc/sched.h"
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

void timer_init_this_cpu(void)
{
	/* Per-CPU GIC interface bring-up + PPI 30 enable.  PPIs are
	 * banked per-CPU but the enable register lives in the
	 * distributor's GICD_ISENABLER0 with a per-CPU view; writing
	 * to it from each CPU is the canonical sequence. */
	gic_cpu_init();
	gic_enable_irq(PLAT_TIMER_PPI);

	write_cntp_tval(tick_cycles);
	write_cntp_ctl(1);	/* ENABLE=1, IMASK=0 */
}

void timer_init(unsigned hz)
{
	uint64_t freq = read_cntfrq();
	tick_cycles = freq / hz;

	/* Distributor bring-up runs once on the boot CPU. */
	gic_dist_init();

	timer_init_this_cpu();

	kprintf("timer: cntfrq=%lu Hz, tick=%u Hz (%lu cycles), via GIC PPI %u\n",
		(unsigned long)freq, hz, (unsigned long)tick_cycles,
		PLAT_TIMER_PPI);
}

void irq_dispatch(void)
{
	uint32_t iar   = gic_iar();
	uint32_t intid = iar & 0x3ff;

	/* T1: EOI BEFORE any potential yield.  GIC v3 keeps the
	 * interrupt "active" until ICC_EOIR1_EL1 is written -- the
	 * running priority stays at the IRQ's priority, which blocks
	 * any equal- or lower-priority interrupt from preempting.
	 * If we ran the handler (which yields) before EOI, the GIC
	 * would never see another timer pending (RPR stuck high), the
	 * scheduler couldn't be re-driven by the tick, and we'd hang
	 * after the first IRQ.  raspi3b doesn't hit this because the
	 * BCM2836 routing block has no EOI concept -- the timer's
	 * CNTP_TVAL re-arm is what deasserts its level. */
	if (intid == PLAT_TIMER_PPI) {
		write_cntp_tval(tick_cycles);
		gic_eoi(iar);
		sched_tick();
		return;
	}
	if (intid == PLAT_IPI_SGI) {
		gic_eoi(iar);
		ipi_handle();
		return;
	}
	if (intid >= 1020) {
		/* spurious / no pending */
		return;
	}
	gic_eoi(iar);
	kprintf("irq: unhandled intid=%u\n", intid);
}

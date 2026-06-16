/*
 * uts/virt/ipi.c -- GIC-based IPI backend
 *
 * Cross-CPU wake using GIC SGI 0.  Single-core today (virt secondaries
 * are PSCI-parked until we wire CPU_ON HVCs), so the send paths are
 * functionally no-ops, but the wiring is in place for when SMP comes.
 */

#include <stdint.h>

#include "kappara/arch/gic.h"
#include "kappara/arch/ipi.h"
#include "kappara/proc/sched.h"
#include "platform.h"

static inline unsigned this_cpu_id(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, mpidr_el1" : "=r"(v));
	return (unsigned)(v & 0xff);
}

void ipi_init_this_cpu(void)
{
	/* SGIs are always enabled at the CPU interface level once
	 * gic_cpu_init() has run.  timer_init_this_cpu() already does
	 * that; nothing further to do here. */
}

void ipi_send(unsigned target_cpu)
{
	if (target_cpu >= 4) return;
	if (target_cpu == this_cpu_id()) return;
	gic_send_sgi(target_cpu, PLAT_IPI_SGI);
}

extern uint32_t sched_idle_mask(void);

void ipi_wake_idle(void)
{
	uint32_t mask = sched_idle_mask();
	unsigned self = this_cpu_id();
	mask &= ~(1u << self);
	while (mask) {
		unsigned cpu = (unsigned)__builtin_ctz(mask);
		mask &= mask - 1u;
		gic_send_sgi(cpu, PLAT_IPI_SGI);
	}
}

void ipi_handle(void)
{
	/* SGI clearing is automatic on EOI (which irq_dispatch will
	 * issue after we return).  Nothing to do beyond letting the
	 * dispatcher re-evaluate switch_to_next / try_steal. */
}

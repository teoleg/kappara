/*
 * arch/aarch64/ipi.c -- BCM2836 mailbox-based IPI backend
 * =======================================================
 *
 * See include/kappara/arch/ipi.h for the contract.
 *
 * Layout reminder (from platform/pi3.h):
 *
 *   MBOX_IRQ_CTL(N)        bit M = enable mailbox M IRQ on core N
 *   MBOX_SET(N, M)         write any value to send IPI to core N mailbox M
 *   MBOX_RDCLR(N, M)       read = current pending; write 1s to clear
 *   IRQ_SOURCE(N) bit 4    mailbox 0 pending on core N
 *
 * One mailbox is enough today: the IPI itself is the signal, no
 * payload.  If we ever need per-IPI reasons (resched vs panic vs
 * tlbi), we get four mailboxes per core to play with.
 */

#include <stdint.h>

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
	unsigned cpu = this_cpu_id();
	/* Enable mailbox 0 -> IRQ on this core.  Leaving mailboxes 1..3
	 * disabled means stray writes to them are harmless. */
	*(volatile uint32_t *)PLAT_MBOX_IRQ_CTL(cpu) = 1u;
}

void ipi_send(unsigned target_cpu)
{
	if (target_cpu >= 4) return;
	if (target_cpu == this_cpu_id()) return;
	/* Any non-zero value sets the mailbox; the receiver only cares
	 * that it became non-zero.  DSB so the peer's MMIO read sees
	 * our write before it returns from its WFI exception. */
	*(volatile uint32_t *)PLAT_MBOX_SET(target_cpu, 0) = 1u;
	__asm__ volatile ("dsb sy" ::: "memory");
}

extern uint32_t sched_idle_mask(void);	/* in kernel/sched.c */

void ipi_wake_idle(void)
{
	uint32_t mask = sched_idle_mask();
	unsigned self = this_cpu_id();
	mask &= ~(1u << self);
	while (mask) {
		unsigned cpu = (unsigned)__builtin_ctz(mask);
		mask &= mask - 1u;
		*(volatile uint32_t *)PLAT_MBOX_SET(cpu, 0) = 1u;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

void ipi_handle(void)
{
	unsigned cpu = this_cpu_id();
	/* Read the pending word, write the same bits back to clear.
	 * On this hardware only bit 0 (our reason for using mailbox 0)
	 * is ever set, so we could just write 1u, but mirroring the
	 * read makes the intent explicit. */
	volatile uint32_t *rdclr =
		(volatile uint32_t *)PLAT_MBOX_RDCLR(cpu, 0);
	uint32_t v = *rdclr;
	*rdclr = v;
	__asm__ volatile ("dsb sy" ::: "memory");
	/* Nothing further to do -- returning from the IRQ vector drops
	 * back into the dispatcher, which will retry switch_to_next /
	 * try_steal. */
}

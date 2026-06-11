/*
 * include/kappara/spinlock.h -- AArch64 ticketless spinlock
 *
 * A flat exclusive-monitor spinlock for SMP synchronization of
 * shared kernel state.  Single-CPU code still benefits: the
 * irq_save/restore variant gives us "disable IRQs around a
 * critical section" cheaply.
 *
 * Implementation
 * --------------
 *   acquire: ldaxr -> cbnz spin -> stxr -> cbnz retry
 *   release: stlr wzr
 *
 * LDAXR is acquire-ordered, STLR is release-ordered, so the
 * normal happens-before story holds without explicit DSBs.  No
 * fairness (not a ticket lock); contention is bounded by the
 * number of CPUs.
 *
 * Use spin_lock_irq_save / spin_unlock_irq_restore when the lock
 * is also touched from an interrupt path (e.g. the scheduler) --
 * otherwise a timer IRQ inside the critical section deadlocks
 * the local CPU.
 */
#ifndef KAPPARA_SPINLOCK_H
#define KAPPARA_SPINLOCK_H

#include <stdint.h>

typedef struct { volatile uint32_t lock; } spinlock_t;

#define SPINLOCK_INIT	{ .lock = 0 }

static inline void spin_lock(spinlock_t *l)
{
	uint32_t tmp1, tmp2;
	__asm__ volatile (
		"1:	ldaxr	%w0, [%2]\n"
		"	cbnz	%w0, 1b\n"
		"	mov	%w0, #1\n"
		"	stxr	%w1, %w0, [%2]\n"
		"	cbnz	%w1, 1b\n"
		: "=&r"(tmp1), "=&r"(tmp2)
		: "r"(&l->lock)
		: "memory", "cc");
}

static inline void spin_unlock(spinlock_t *l)
{
	__asm__ volatile ("stlr wzr, [%0]"
			  : : "r"(&l->lock) : "memory");
}

/* Save current DAIF + mask IRQs + take the lock.  Returns the
 * previous DAIF.  Pair with spin_unlock_irq_restore. */
static inline unsigned long spin_lock_irq_save(spinlock_t *l)
{
	unsigned long flags;
	__asm__ volatile ("mrs %0, daif" : "=r"(flags));
	__asm__ volatile ("msr daifset, #2" ::: "memory");
	spin_lock(l);
	return flags;
}

static inline void spin_unlock_irq_restore(spinlock_t *l, unsigned long flags)
{
	spin_unlock(l);
	__asm__ volatile ("msr daif, %0" :: "r"(flags) : "memory");
}

#endif

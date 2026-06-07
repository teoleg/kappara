/*
 * include/kappara/thread_lock.h -- Solaris polymorphic thread-state lock
 * ======================================================================
 *
 * Each kthread carries `t_lockp` -- a pointer to whichever spinlock
 * currently owns its mutable state.  As the thread moves between
 * dispatch queue / sleep queue / per-CPU ownership, t_lockp is
 * updated to point at the new owner.  Code that mutates thread state
 * MUST first acquire *t_lockp via thread_lock(), which spins through
 * pointer changes mid-acquire (one of the well-known idioms of the
 * Solaris dispatcher; see illumos `usr/src/uts/common/disp/thread.c`,
 * function `thread_lock`).
 *
 * This header defines the locking primitives.  The transition rules --
 * which lock owns a thread in which state -- live in the comment on
 * `struct kthread::t_lockp` in include/kappara/sched.h.
 *
 * Phase 1 (introductory commit): these primitives exist and the per-
 * thread/per-CPU locks are initialised, but nothing reads t_lockp
 * yet.  Phase 2 wires swtch().
 */
#ifndef KAPPARA_THREAD_LOCK_H
#define KAPPARA_THREAD_LOCK_H

#include "kappara/sched.h"
#include "kappara/spinlock.h"

/*
 * Sentinel value for `kthread.t_lockp` during a thread_lock_transfer
 * (added in a later phase).  Reserved here so the API surface is
 * stable from phase 1.  Nobody calls spin_lock on this -- it's just a
 * unique non-NULL pointer that signals "in flux" to retrying
 * thread_lock callers.
 */
extern spinlock_t t_transition_lock;

/*
 * thread_lock(t) -- acquire whatever lock currently owns t's state.
 *
 * Spin reading t_lockp, lock that, then verify t_lockp hasn't
 * changed during the acquire (if it did, another CPU finished a
 * transfer mid-spin -- drop and retry).  Idiomatic Solaris.
 */
static inline void thread_lock(struct kthread *t)
{
	for (;;) {
		spinlock_t *lp = t->t_lockp;
		if (lp == &t_transition_lock) {
			__asm__ volatile ("yield" ::: "memory");
			continue;
		}
		spin_lock(lp);
		if (lp == t->t_lockp)
			return;
		spin_unlock(lp);
	}
}

/*
 * thread_unlock(t) -- release the lock currently in *t_lockp.
 * Caller must have acquired it via thread_lock(t).
 */
static inline void thread_unlock(struct kthread *t)
{
	spin_unlock(t->t_lockp);
}

/*
 * thread_lock_high(t) / thread_unlock_high() -- IRQ-disabling pair.
 * Returns saved DAIF; pass the same value back into _high() at unlock.
 *
 * Used by callers that arrive from a non-IRQ-masked context (signal
 * delivery, /proc/ps iteration) and need to atomically mask + lock.
 */
static inline unsigned long thread_lock_high(struct kthread *t)
{
	unsigned long flags;
	__asm__ volatile ("mrs %0, daif" : "=r"(flags));
	__asm__ volatile ("msr daifset, #2" ::: "memory");
	thread_lock(t);
	return flags;
}

static inline void thread_unlock_high(struct kthread *t, unsigned long flags)
{
	thread_unlock(t);
	__asm__ volatile ("msr daif, %0" :: "r"(flags) : "memory");
}

#endif

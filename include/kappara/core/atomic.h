/*
 * include/kappara/atomic.h -- single-word lock-free counters
 * ==========================================================
 *
 * Just enough atomic primitives for refcount-style use: a fenced
 * increment and a fenced "decrement and test for zero".  Built on
 * LDAXR/STLXR exclusive-monitor pairs, the same shape as spinlock.h.
 *
 * What this is for
 * ----------------
 * Reference counters that are touched from multiple CPUs.  The
 * canonical examples in this tree are struct file::f_refs and struct
 * inode::i_count -- both can be reached from any CPU that holds (or
 * is about to drop) an fd slot, and concurrent dup + close across
 * CPUs will race on them.
 *
 * The contract callers rely on:
 *
 *   atomic_inc(p)
 *       Increment *p.  ACQUIRE semantics: any read the caller does
 *       through whatever object the count protects (after this
 *       returns) sees the writes the previous reference holder
 *       published before they bumped the count themselves.
 *
 *   atomic_dec_and_test(p)
 *       Decrement *p.  RELEASE semantics: any write the caller did
 *       through the protected object before this call is visible to
 *       whoever observes the lower count.  Returns 1 if the result
 *       is zero (the caller drove the last reference away and is
 *       the unique cleanup observer); 0 otherwise.
 *
 *       The decrement-to-zero path emits an additional load-acquire
 *       barrier so the caller's subsequent cleanup reads through *p
 *       see the fully-consistent state that every other CPU's
 *       prior RELEASE-dec made visible.  This pairs the standard
 *       "deferred cleanup at refcount 0" pattern Linux's
 *       refcount.h, Solaris's atomic_dec_uint_nv, and FreeBSD's
 *       refcount_release all use.
 *
 * Why hand-rolled in inline asm
 * -----------------------------
 * AArch64 GCC >= 10 defaults to "outline atomics" for C11
 * __atomic_* builtins: at -O2 it emits calls to libgcc helpers
 * (__aarch64_ldadd4_acq_rel, ...) which dispatch to LSE vs.
 * LDXR/STXR at runtime through a constructor that calls
 * __getauxval().  We're freestanding -- no glibc, no auxv -- so
 * dragging libgcc into the trust boundary just to get a 32-bit
 * counter add would be the wrong trade.  Emit the exclusive-monitor
 * loop directly instead; the result is one cache-line touch and
 * three instructions in the uncontended case, with no symbol
 * dependency outside the kernel.
 *
 * Why ACQUIRE on inc and RELEASE on dec (not full barrier on both)
 * ----------------------------------------------------------------
 * That's the minimum ordering the refcount-of-object pattern
 * actually needs.  LDAXR provides acquire on the load; STLXR
 * provides release on the store.  Pairing them in the same RMW
 * (LDAXR / op / STLXR) on AArch64 yields an ACQ_REL operation,
 * which is slightly stronger than needed but free in practice on
 * Cortex-A53.  The shape matches spinlock.h's ldaxr / stxr (where
 * we use STXR, not STLXR, because the lock release does the STLR
 * separately).
 */
#ifndef KAPPARA_ATOMIC_H
#define KAPPARA_ATOMIC_H

#include <stdint.h>

/*
 * Atomic 32-bit signed increment.  ACQUIRE semantics through LDAXR
 * on the load half of the read-modify-write; the STLXR on the store
 * half is release-ordered but the typical caller (atomic_inc) does
 * not require release on the inc path, so the extra cost is one
 * unnecessary release barrier per inc.  On Cortex-A53 that's nil.
 */
static inline void atomic_inc(int *p)
{
	int tmp, succ;
	__asm__ volatile (
		"1:	ldaxr	%w0, [%2]\n"
		"	add	%w0, %w0, #1\n"
		"	stlxr	%w1, %w0, [%2]\n"
		"	cbnz	%w1, 1b\n"
		: "=&r"(tmp), "=&r"(succ)
		: "r"(p)
		: "memory");
}

/*
 * Atomic 32-bit signed decrement-and-test-for-zero.  Returns 1 if
 * the post-decrement value is 0 (caller is the last reference and
 * owns the cleanup), 0 otherwise.  RELEASE on the store half via
 * STLXR; an extra load-acquire barrier on the dec-to-zero path
 * pairs with every other CPU's prior RELEASE drops so the cleanup
 * code sees a fully-consistent view of whatever *p protects.
 */
static inline int atomic_dec_and_test(int *p)
{
	int newval, succ;
	__asm__ volatile (
		"1:	ldaxr	%w0, [%2]\n"
		"	sub	%w0, %w0, #1\n"
		"	stlxr	%w1, %w0, [%2]\n"
		"	cbnz	%w1, 1b\n"
		: "=&r"(newval), "=&r"(succ)
		: "r"(p)
		: "memory");
	if (newval == 0) {
		/* dmb ishld: inner-shareable load-acquire barrier.
		 * Pairs with the STLXR releases on every other CPU's
		 * prior decrement so our subsequent cleanup reads
		 * through *p see a coherent state.  Without this the
		 * compiler / CPU is free to hoist a *p load above the
		 * STLXR and observe stale values written by other CPUs
		 * before they released. */
		__asm__ volatile ("dmb ishld" ::: "memory");
		return 1;
	}
	return 0;
}

#endif

/*
 * kernel/signal.c -- DEC/BSD-style reliable signal delivery
 *
 * Bottom layer (sys_kill + pending bitmap + check_signals at trap
 * return): unchanged in shape since the first signal commit.  This
 * file now adds the top layer: per-thread `struct sigaction_k`,
 * sendsig() to push a signal context onto the user stack and
 * redirect ERET into a user handler, and sigreturn to unwind that
 * frame back into the saved trap state.
 *
 * Disposition ladder for the lowest deliverable signal:
 *
 *   SIGKILL          -> always exit, never blockable, never catchable.
 *   sa_handler==SIG_DFL  ->  fatal? exit.  else drop.
 *   sa_handler==SIG_IGN  ->  drop, clear pending bit.
 *   any other handler    ->  sendsig() builds a sigframe on the user
 *                            stack, rewrites tf so ERET vectors into
 *                            the handler with x0=signo and x30 pointing
 *                            at a kernel-emitted trampoline that issues
 *                            SYS_sigreturn.
 *
 * The trampoline lives inside the sigframe itself.  The user binary's
 * 2 MB region is mapped RWX (see arch/aarch64/mmu.c) so executing
 * stack-resident instructions is legal here -- not portable, but for
 * a learning OS without a vDSO it keeps the design self-contained.
 * One DC CVAU + IC IVAU + DSB + ISB per frame keeps real hardware
 * (QEMU TCG doesn't care) coherent across the I-side.
 */

#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/signal.h"
#include "kappara/trap.h"
#include "kappara/uaccess.h"
#include "kappara/user.h"	/* sys_exit_impl */
#include "kappara/kmem.h"

extern void *kmalloc(size_t size);
extern void  kmemset(void *dst, int v, size_t n);

/*
 * The on-stack signal frame.  Layout is deliberately fixed so that
 * sigreturn's restore step is a single copy_from_user.  Everything
 * sendsig writes here, sigreturn reads back.
 */
struct sigframe {
	uint32_t trampoline[2];	/* MOVZ x8,#SYS_sigreturn ; SVC #0 */
	uint32_t signo;		/* handler's first arg, also for debug */
	uint32_t saved_mask;	/* sig_mask before the handler call    */
	uint64_t saved_x[31];	/* x0..x30 from the interrupted state  */
	uint64_t saved_sp_el0;
	uint64_t saved_elr;
	uint64_t saved_spsr;
};

int sys_kill_impl(int tid, int sig)
{
	if (tid <= 0 || sig < 0 || sig >= NSIG) return -1;
	struct kthread *t = kthread_find((unsigned)tid);
	if (!t) {
		kprintf("sys_kill: no thread with tid=%d\n", tid);
		return -1;
	}
	/* sig == 0 is the POSIX "tid alive?" probe -- we found the
	 * thread, return 0 without doing anything else. */
	if (sig == 0) return 0;
	return kthread_signal(t, (unsigned)sig);
}

long sys_sigaction_impl(int sig,
			const struct sigaction_k *uact,
			struct sigaction_k *uoldact)
{
	struct kthread *t = curthread;
	if (!t) return -1;
	if (sig <= 0 || sig >= NSIG) return -1;
	if (sig == SIGKILL) return -1;	/* uncatchable */

	/* Lazy-allocate the dispositions table on first use.  A thread
	 * that never installs a handler pays nothing in slab. */
	if (!t->sig_actions) {
		struct sigaction_k *tbl = kmalloc(sizeof(*tbl) * NSIG);
		if (!tbl) return -1;
		kmemset(tbl, 0, sizeof(*tbl) * NSIG);
		t->sig_actions = tbl;
	}

	struct sigaction_k old = t->sig_actions[sig];

	if (uact) {
		struct sigaction_k new;
		if (copy_from_user(&new, uact, sizeof(new)) < 0) return -1;
		t->sig_actions[sig] = new;
	}
	if (uoldact) {
		if (copy_to_user(uoldact, &old, sizeof(old)) < 0) return -1;
	}
	return 0;
}

/*
 * Build a signal frame at the top of the user stack and rewrite tf
 * so ERET lands in the user handler.  Caller has already verified
 * that `handler` is a real user handler (not SIG_DFL / SIG_IGN).
 *
 * Returns 0 on success; -1 if the user stack pointer is bogus, in
 * which case the caller should fall back to the default action
 * (typically: kill the thread).
 */
static int sendsig(struct trap_frame *tf, int sig, uint64_t handler)
{
	uint64_t sp = tf->sp_el0;
	if (sp < sizeof(struct sigframe)) return -1;	/* underflow guard */
	sp -= sizeof(struct sigframe);
	sp &= ~0xFUL;				/* 16-byte align */
	if (!user_ptr_ok((void *)(uintptr_t)sp, sizeof(struct sigframe)))
		return -1;

	struct sigframe fr;
	/*
	 * Trampoline: two AArch64 instructions.
	 *   MOVZ x8, #SYS_sigreturn
	 *   SVC  #0
	 * MOVZ Xd, #imm16 encoding: 0xD2800000 | (imm << 5) | Rd.
	 * Rd=8, imm=23 -> 0xD2800000 | 0x2E0 | 0x8 = 0xD28002E8.
	 */
	fr.trampoline[0] = 0xD28002E8u;
	fr.trampoline[1] = 0xD4000001u;
	fr.signo         = (uint32_t)sig;
	/*
	 * If we're delivering on the way out of sigsuspend(2), the
	 * mask sigreturn must unwind to is the PRE-sigsuspend mask,
	 * not the temporary one currently in sig_mask.  Consume the
	 * save-pending flag here so a subsequent check_signals pass
	 * (after the handler eventually returns and sigreturn brings
	 * us back) sees a clean state. */
	if (curthread->sig_mask_save_pending) {
		fr.saved_mask = curthread->sig_saved_mask;
		curthread->sig_mask_save_pending = 0;
	} else {
		fr.saved_mask = curthread->sig_mask;
	}
	for (int i = 0; i < 31; i++) fr.saved_x[i] = tf->x[i];
	fr.saved_sp_el0  = tf->sp_el0;
	fr.saved_elr     = tf->elr;
	fr.saved_spsr    = tf->spsr;

	if (copy_to_user((void *)(uintptr_t)sp, &fr, sizeof(fr)) < 0)
		return -1;

	/*
	 * We just wrote new instructions into a page the user is about
	 * to execute.  AArch64 separates I- and D-cache; without the
	 * dance below the CPU may fetch stale bytes from the I-cache.
	 * QEMU TCG happens to see-through, real hardware doesn't.
	 */
	uint64_t tramp_addr = sp + (uint64_t)__builtin_offsetof(struct sigframe, trampoline);
	__asm__ volatile (
		"dc  cvau, %0    \n"
		"dsb ish         \n"
		"ic  ivau, %0    \n"
		"dsb ish         \n"
		"isb             \n"
		: : "r"(tramp_addr) : "memory");

	/* Rewrite trap frame for ERET into the handler. */
	tf->x[0]   = (uint64_t)(uint32_t)sig;	/* handler arg */
	tf->x[30]  = tramp_addr;		/* return address = trampoline */
	tf->elr    = handler;
	tf->sp_el0 = sp;
	return 0;
}

long sys_sigreturn_impl(struct trap_frame *tf)
{
	struct kthread *t = curthread;
	if (!t) return -1;
	uint64_t sp = tf->sp_el0;
	if (!user_ptr_ok((void *)(uintptr_t)sp, sizeof(struct sigframe)))
		return -1;

	struct sigframe fr;
	if (copy_from_user(&fr, (void *)(uintptr_t)sp, sizeof(fr)) < 0)
		return -1;

	for (int i = 0; i < 31; i++) tf->x[i] = fr.saved_x[i];
	tf->sp_el0 = fr.saved_sp_el0;
	tf->elr    = fr.saved_elr;
	tf->spsr   = fr.saved_spsr;
	t->sig_mask = fr.saved_mask;

	/*
	 * Returning tf->x[0] from this function would make the SVC
	 * return path overwrite x[0] with what we just restored,
	 * which is exactly what we want -- the saved x[0] becomes
	 * the resumed thread's x[0] without an extra store.
	 */
	return (long)tf->x[0];
}

long sys_sigprocmask_impl(int how, const uint32_t *uset, uint32_t *uoldset)
{
	struct kthread *t = curthread;
	if (!t) return -1;

	uint32_t old = t->sig_mask;
	if (uoldset) {
		if (copy_to_user(uoldset, &old, sizeof(old)) < 0) return -1;
	}
	if (!uset) return 0;

	uint32_t set;
	if (copy_from_user(&set, uset, sizeof(set)) < 0) return -1;
	/* SIGKILL is silently filtered out -- POSIX guarantees it can
	 * never be blocked.  No EINVAL since the user expects to be
	 * able to pass a full-fills mask without an error. */
	set &= ~SIGBIT(SIGKILL);

	switch (how) {
	case SIG_BLOCK:   t->sig_mask = old | set;  break;
	case SIG_UNBLOCK: t->sig_mask = old & ~set; break;
	case SIG_SETMASK: t->sig_mask = set;        break;
	default: return -1;
	}
	return 0;
}

long sys_sigsuspend_impl(uint32_t mask)
{
	/*
	 * Atomically install the temporary mask, sleep until a
	 * deliverable signal lands, return -1.  The mask restore is
	 * deferred: check_signals takes care of it via sig_saved_mask
	 * either by handing it to sendsig (so sigreturn unwinds to
	 * the pre-call value) or by writing it directly back into
	 * sig_mask if no handler ran.  Doing the restore here would
	 * re-block the very signal we're about to dispatch -- the bug
	 * that took the first masktest down.
	 *
	 * We sleep on thread_exit_wq even though we're not waiting
	 * for a thread exit -- it's a convenient broadcast queue,
	 * and kthread_signal pulls us off it surgically the moment
	 * a signal arrives.  An incidental wake from someone else's
	 * exit just retries the loop, which is harmless.
	 */
	struct kthread *t = curthread;
	if (!t) return -1;

	t->sig_saved_mask         = t->sig_mask;
	t->sig_mask_save_pending  = 1;
	t->sig_mask               = mask & ~SIGBIT(SIGKILL);

	while (!(t->sig_pending & ~t->sig_mask)) {
		kthread_sleep_on(&thread_exit_wq);
	}

	return -1;	/* check_signals handles delivery + mask unwind */
}

long sys_wait_impl(int tid)
{
	if (tid <= 0) return -1;
	for (;;) {
		/* kthread_find returns NULL for both "never existed" and
		 * "exited (KT_DEAD or already reaped)" -- both are "tid
		 * is gone, return 0".  We don't need to distinguish here
		 * since we don't expose a status/exit-code yet. */
		struct kthread *t = kthread_find((unsigned)tid);
		if (!t) return 0;

		struct kthread *me = curthread;
		if (me && (me->sig_pending & SIG_FATAL_MASK))
			return -1;	/* EINTR shape */

		kthread_sleep_on(&thread_exit_wq);
	}
}

void check_signals(struct trap_frame *tf)
{
	struct kthread *t = curthread;
	if (!t) return;

	for (;;) {
		/*
		 * SIGKILL bypasses the mask entirely.  Everything else
		 * obeys it.  We deliver one signal per trap-return pass;
		 * if there's more pending we'll come back next syscall.
		 */
		uint32_t deliverable = (t->sig_pending & ~t->sig_mask)
				     | (t->sig_pending & SIGBIT(SIGKILL));
		if (!deliverable) {
			/*
			 * Nothing left to deliver.  If sigsuspend stashed
			 * a mask to restore but no handler ran (e.g. the
			 * waking signal was SIG_IGN'd), put the mask back
			 * here so the user-visible sig_mask matches the
			 * POSIX semantics.
			 */
			if (t->sig_mask_save_pending) {
				t->sig_mask = t->sig_saved_mask;
				t->sig_mask_save_pending = 0;
			}
			return;
		}

		unsigned sig;
		for (sig = 1; sig < NSIG; sig++)
			if (deliverable & SIGBIT(sig)) break;
		if (sig >= NSIG) return;

		/* Consume the pending bit before dispatching so a
		 * handler that does its own sys_kill doesn't see this
		 * one re-trigger. */
		t->sig_pending &= ~SIGBIT(sig);

		uint64_t handler = (sig == SIGKILL)
				 ? SIG_DFL
				 : (t->sig_actions
					? t->sig_actions[sig].sa_handler
					: SIG_DFL);

		if (handler == SIG_DFL) {
			if (SIG_FATAL_MASK & SIGBIT(sig)) {
				kprintf("kthread: tid=%u killed by signal %u\n",
					t->tid, sig);
				sys_exit_impl();
				/* unreachable */
			}
			/* non-fatal default = ignore */
			continue;
		}
		if (handler == SIG_IGN)
			continue;

		/*
		 * User handler.  Build the frame on the user stack and
		 * mask this signal (plus the caller-requested mask)
		 * while the handler runs.  sigreturn will restore.
		 *
		 * Capture sa_flags BEFORE sendsig (which may consume the
		 * mask-save state) so we can honour SA_RESETHAND
		 * afterwards: reset the disposition to SIG_DFL so a
		 * second occurrence -- e.g. the handler returning to a
		 * re-faulting instruction in the EL0 SIGSEGV path --
		 * kills the thread instead of looping.
		 */
		uint32_t sa_flags = t->sig_actions[sig].sa_flags;
		uint32_t sa_mask  = t->sig_actions[sig].sa_mask;
		if (sendsig(tf, (int)sig, handler) < 0) {
			kprintf("kthread: tid=%u sendsig(%u) failed\n",
				t->tid, sig);
			sys_exit_impl();
			/* unreachable */
		}
		if (sa_flags & SA_RESETHAND) {
			t->sig_actions[sig].sa_handler = SIG_DFL;
		}
		t->sig_mask |= sa_mask | SIGBIT(sig);
		return;	/* one delivery per pass */
	}
}

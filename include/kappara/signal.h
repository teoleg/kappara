/*
 * include/kappara/signal.h -- BSD/DEC-style signals with POSIX numbering
 *
 * Numbering matches the POSIX/Linux convention so anything we ever
 * port can rely on the usual SIGTERM=15, SIGKILL=9, ...  layout.
 * Numbers below were the de-facto standard by 4.3BSD on the VAX and
 * survived unchanged into SVR4 + POSIX.1.
 *
 * Delivery model (DEC/BSD reliable signals)
 * -----------------------------------------
 * Each thread carries:
 *
 *   sig_pending    bitmap of signals set but not yet delivered
 *   sig_mask       bitmap of signals temporarily blocked
 *   sig_action[]   per-signal disposition: SIG_DFL / SIG_IGN / handler
 *
 * sys_kill(tid, sig) just sets the bit in the target's sig_pending
 * and wakes the thread if it's parked on a wait queue.  Delivery
 * happens on the return-to-user path (check_signals from
 * trap_dispatch's SVC handler) where issig() scans pending&~mask,
 * picks the lowest bit, and either takes the default action
 * (terminate / ignore) or rewrites the trap frame to vector into a
 * user-space handler -- the same mechanism 4BSD's sendsig() used on
 * the VAX, just with an AArch64 trap frame instead of a VAX PSL+PC
 * sigcontext.
 *
 * For now the implementation only handles defaults: a fatal signal
 * makes the thread call sys_exit on its next syscall return.  User
 * handlers (sigaction + sendsig + sigreturn) land in the next pass.
 */

#ifndef KAPPARA_SIGNAL_H
#define KAPPARA_SIGNAL_H

#include <stdint.h>

struct trap_frame;	/* fwd; defined in trap.h */

/* POSIX signal numbers.  Gaps are intentional: SIGEMT (7), SIGBUS (10),
 * SIGUSR1/2 (16/17 on Linux, 30/31 on BSD), etc. -- we just leave the
 * slots reserved for whenever they're needed. */
#define SIGHUP		1
#define SIGINT		2
#define SIGQUIT		3
#define SIGILL		4
#define SIGTRAP		5
#define SIGABRT		6
#define SIGFPE		8
#define SIGKILL		9
#define SIGSEGV		11
#define SIGPIPE		13
#define SIGALRM		14
#define SIGTERM		15

#define NSIG		32	/* one past the largest signal number */

/* Bit position of signal N in a uint32_t bitmap.  Signals are 1-based;
 * bit (N-1) carries the pending/mask flag for SIGN. */
#define SIGBIT(n)	(1u << ((n) - 1))

/* The mask of signals whose default action is "terminate".  Everything
 * else either coredumps (we don't have cores yet) or is ignored.  Note
 * that SIGKILL is in this set AND cannot be masked or caught -- the
 * delivery path enforces both. */
#define SIG_FATAL_MASK	(SIGBIT(SIGHUP)  | SIGBIT(SIGINT)  | \
			 SIGBIT(SIGQUIT) | SIGBIT(SIGKILL) | \
			 SIGBIT(SIGPIPE) | SIGBIT(SIGTERM) | \
			 SIGBIT(SIGSEGV) | SIGBIT(SIGABRT))

/* Magic sa_handler values, kept in the same encoding as the user's
 * `void (*)(int)` so the dispatch can compare directly.  A 0 / 1
 * pointer is never a valid user code address (user VA starts at
 * 0x10000000), so these are unambiguous. */
#define SIG_DFL		0UL
#define SIG_IGN		1UL

/* `how` values for sigprocmask. */
#define SIG_BLOCK	0
#define SIG_UNBLOCK	1
#define SIG_SETMASK	2

/* sa_flags bits.  Only one defined so far; reserved space for the
 * common POSIX flags (SA_NODEFER, SA_SIGINFO, SA_RESTART) when we
 * want them. */
#define SA_RESETHAND	0x04	/* reset to SIG_DFL after first delivery */

/*
 * Kernel-side per-signal disposition.  Same layout as the user-visible
 * `struct sigaction` -- 16 bytes, naturally aligned -- so we can copy
 * the user buffer in with one memcpy.
 */
struct sigaction_k {
	uint64_t  sa_handler;	/* user VA; SIG_DFL or SIG_IGN allowed */
	uint32_t  sa_mask;	/* signals blocked across handler call */
	uint32_t  sa_flags;	/* reserved -- pass 0 for now */
};

/* sys_kill(tid, sig).  Returns 0 on success or -1 if tid not found
 * or sig out of range.  sig==0 is the POSIX "just probe whether the
 * tid exists" form; for symmetry we accept it but it never wakes. */
int sys_kill_impl(int tid, int sig);

/* sys_sigaction(sig, act, oldact): install a per-thread disposition.
 * SIGKILL is rejected (cannot be caught or ignored, BSD/POSIX). */
long sys_sigaction_impl(int sig,
			const struct sigaction_k *uact,
			struct sigaction_k *uoldact);

/* sys_sigreturn: restore the saved trap frame at the top of the user
 * stack and unwind the signal mask.  Called from the trampoline that
 * sendsig() built into the signal frame; not exposed to user code as
 * a callable wrapper.  Implemented inside trap.c's dispatch path
 * because it needs to mutate tf in place. */
long sys_sigreturn_impl(struct trap_frame *tf);

/* sys_sigprocmask: change the calling thread's signal-block mask
 * according to `how` (SIG_BLOCK | SIG_UNBLOCK | SIG_SETMASK).  Both
 * pointer args may be NULL.  SIGKILL is silently filtered out of any
 * incoming mask -- it cannot be blocked, BSD/POSIX guarantee. */
long sys_sigprocmask_impl(int how, const uint32_t *uset, uint32_t *uoldset);

/* sys_sigsuspend: atomically replace the calling thread's mask with
 * `mask`, sleep until a deliverable signal lands, then restore the
 * previous mask just before returning -1.  The standard atomic-wait
 * primitive that race-free signal handling needs. */
long sys_sigsuspend_impl(uint32_t mask);

/* sys_wait: block until tid has exited (or return immediately if the
 * tid is already gone).  Returns 0 on a clean observation, -1 if tid
 * is malformed or the wait was interrupted by a fatal signal.  Lives
 * in kernel/signal.c next to the other thread-state syscalls. */
long sys_wait_impl(int tid);

/* SVR4 session / pgrp + controlling-tty foreground group.  See
 * sys_setpgid_impl and friends in kernel/signal.c for the bodies. */
long sys_setpgid_impl(int pid, int pgid);
long sys_getpgrp_impl(void);
long sys_setsid_impl(void);
long sys_tcsetpgrp_impl(int fd, int pgid);
long sys_tcgetpgrp_impl(int fd);

/* Trap-return signal delivery point.  Called from the SVC handler
 * after the syscall's impl returns and before ERET.  Walks the
 * deliverable bits in pending&~mask, picks the lowest, and either:
 *
 *   - takes the default action (terminate for SIG_FATAL_MASK signals,
 *     drop everything else),
 *   - drops on SIG_IGN,
 *   - calls sendsig() to rewrite tf so ERET vectors into the user
 *     handler with a sigframe pushed onto the user stack.
 *
 * SIGKILL is delivered unconditionally and always takes the default
 * action -- it cannot be masked or caught. */
void check_signals(struct trap_frame *tf);

#endif

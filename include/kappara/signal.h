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

/* sys_kill(tid, sig).  Returns 0 on success or -1 if tid not found
 * or sig out of range.  sig==0 is the POSIX "just probe whether the
 * tid exists" form; for symmetry we accept it but it never wakes. */
int sys_kill_impl(int tid, int sig);

/* Called from the trap return path; if cur has a fatal signal
 * pending and it's not blocked, exit the thread.  Otherwise no-op. */
void check_signals(void);

#endif

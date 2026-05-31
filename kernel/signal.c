/*
 * kernel/signal.c -- DEC/BSD-style reliable signal delivery
 *
 * sys_kill sets the pending bit on the target thread (and unblocks
 * it from any wait queue so it observes the signal).  check_signals
 * runs on the trap-return path; for now there's no user-handler
 * machinery -- every fatal signal turns into a sys_exit on the way
 * out, the same default 4.3BSD took for an unhandled SIGTERM/SIGHUP.
 *
 * The full DEC model (sigaction + sendsig + sigreturn for catchable
 * signals on a user-supplied handler) is the next layer on top of
 * this; the bookkeeping below already supports it -- check_signals
 * will sprout a branch for non-default actions when that lands.
 */

#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/signal.h"
#include "kappara/user.h"	/* sys_exit_impl */

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

void check_signals(void)
{
	if (!cur) return;
	uint32_t pending = cur->sig_pending;
	if (!pending) return;

	/* SIGKILL is never blockable or catchable.  Everything else
	 * (until we add sigaction + masks) is delivered at the default
	 * disposition: fatal signals exit the thread, the rest are
	 * silently ignored. */
	if (pending & SIG_FATAL_MASK) {
		/* Find the lowest fatal signal bit for the log line. */
		unsigned sig;
		for (sig = 1; sig < NSIG; sig++)
			if ((pending & SIG_FATAL_MASK) & SIGBIT(sig)) break;
		kprintf("kthread: tid=%u killed by signal %u\n",
			cur->tid, sig);
		sys_exit_impl();
		/* unreachable */
	}
	/* Non-fatal pending bits get cleared so we don't keep checking
	 * them.  Once sigaction lands they'll dispatch instead. */
	cur->sig_pending = 0;
}

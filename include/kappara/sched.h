/*
 * include/kappara/sched.h -- kernel thread + scheduler API
 *
 * kappara's threading model is intentionally tiny:
 *
 *   * One global ready queue, round-robin, no priorities yet.
 *   * One per-thread 4 KB kernel stack (allocated by pmm).
 *   * Cooperative kthread_yield() + preemptive sched_tick() (called
 *     from the timer IRQ handler) both end up doing the same thing.
 *
 * cur is the globally-visible "currently running thread" pointer.
 * For now it's used only by kthread_exit() to identify what just
 * died, but anything that needs current-thread state (a future
 * sleep queue, kalarm, errno-equivalent) will reach for it.
 */
#ifndef KAPPARA_SCHED_H
#define KAPPARA_SCHED_H

enum kt_state {
	KT_READY = 0,
	KT_RUNNING,
	KT_BLOCKED,
};

/* Per-thread fd table size.  Stays in lockstep with FD_MAX in vfs.c;
 * kept here to avoid pulling all of vfs.h into sched.h. */
#define KT_FD_MAX	16

struct file;	/* fwd; defined in vfs.h */

struct kthread {
	void          *sp;		/* saved kernel SP for context switch */
	void          *stack_base;	/* page allocated for the kernel stack */
	const char    *name;
	unsigned       tid;
	enum kt_state  state;
	struct kthread *next;
	/* Per-thread open files.  Each entry is either NULL or a
	 * struct file * whose f_refs counts how many fd slots (across
	 * all threads) still point at it.  See kernel/vfs.c. */
	struct file   *fdt[KT_FD_MAX];
};

extern struct kthread *cur;

void            sched_init(void);
struct kthread *kthread_create(const char *name, void (*fn)(void *), void *arg);
void            kthread_yield(void);
void            kthread_exit(void) __attribute__((noreturn));
void            sched_tick(void);

/* Make `child` inherit `parent`'s open files: each non-NULL slot is
 * copied over and the file's refcount is bumped so closing in one
 * thread doesn't pull the rug out from under the other.  Used by
 * sys_spawn so a worker thread sees the fds its parent set up
 * (pipes, console, ...). */
void            kthread_inherit_fds(struct kthread *child,
				    const struct kthread *parent);

/* ---- Wait queues -----------------------------------------------------
 *
 * A wait_queue is a singly linked list of threads parked on some
 * condition (data on a stream-head read queue, timer expiry, ...).
 * kthread_sleep_on flips the caller from RUNNING to BLOCKED, threads
 * it onto wq, and yields without re-queuing -- so the scheduler skips
 * over the thread entirely until something wakes it.
 *
 * kthread_wake_all / _one pulls waiters off wq, marks them READY, and
 * pushes them back onto the run queue.  The freshly-woken thread
 * resumes inside sleep_on and loops to re-check its condition (think
 * "while empty, sleep" rather than "if empty, sleep") so spurious or
 * coalesced wakeups don't matter.
 *
 * Single-CPU concurrency: the primitives disable IRQs around the
 * state mutation so the timer can't preempt between marking us
 * BLOCKED and the context switch.  Spin-locking lands when SMP does.
 */
struct wait_queue {
	struct kthread *head;
};

#define WAIT_QUEUE_INIT		{ .head = NULL }

void            kthread_sleep_on (struct wait_queue *wq);
void            kthread_wake_all (struct wait_queue *wq);
void            kthread_wake_one (struct wait_queue *wq);

/*
 * Arch-specific hook implemented in arch/<arch>/thread.c.  Lays
 * down the synthetic saved-register frame at stack_top so that
 * the first context_switch into the new thread pops fn/arg into
 * the callee-saved registers and "returns" into thread_trampoline.
 * Returns the SP value the thread should resume at.
 */
void *arch_thread_init_frame(void *stack_top, void (*fn)(void *), void *arg);

#endif

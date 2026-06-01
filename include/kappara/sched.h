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

#include <stdint.h>

#include "kappara/spinlock.h"

enum kt_state {
	KT_READY = 0,
	KT_RUNNING,
	KT_BLOCKED,
	KT_DEAD,	/* kthread_exit called; awaiting reap by another
			 * thread (we can't free the stack we're standing
			 * on -- the next switch_to_next picks the body
			 * off the to-reap list once we're safely off it). */
};

/* Per-thread fd table size.  Stays in lockstep with FD_MAX in vfs.c;
 * kept here to avoid pulling all of vfs.h into sched.h. */
#define KT_FD_MAX	16

struct file;		/* fwd; defined in vfs.h */
struct wait_queue;	/* fwd; defined later in this file */

struct kthread {
	void          *sp;		/* saved kernel SP for context switch */
	void          *stack_base;	/* page allocated for the kernel stack */
	const char    *name;
	unsigned       tid;
	enum kt_state  state;
	struct kthread *next;
	/* Pending-signal bitmap.  sys_kill sets bits, check_signals
	 * (from trap_dispatch's SVC return) consumes them.  See
	 * include/kappara/signal.h for the layout and SIGBIT(). */
	uint32_t       sig_pending;
	/* The wait queue this thread is parked on, or NULL.  Lets
	 * sys_kill surgically extract a sleeping thread from its
	 * queue so it wakes and observes the signal. */
	struct wait_queue *waiting_on;
	/* Per-thread open files.  Each entry is either NULL or a
	 * struct file * whose f_refs counts how many fd slots (across
	 * all threads) still point at it.  See kernel/vfs.c. */
	struct file   *fdt[KT_FD_MAX];
};

/*
 * SVR4-style per-CPU dispatcher state -- one struct cpu per core.
 *
 * Solaris terminology: this is the kernel's `cpu_t`.  Each CPU has
 * its own dispatch queue (so the scheduler doesn't contend on a
 * single global ready queue), its own idle thread, and -- because
 * STREAMS service work is naturally local to whichever CPU
 * qenable'd it -- its own service-procedure runqueue.  The
 * `cpu_thread` field is the per-CPU "currently running" -- what we
 * used to call `cur` and still expose as `curthread`.
 *
 * Lookup is via TPIDR_EL1: each core writes the address of its
 * own `struct cpu` there at bring-up time, then `curcpu()` is one
 * MRS instruction.  `curthread` is sugar for `curcpu()->cpu_thread`.
 *
 * The cpu_disp_lock guards cpu_dispq + the queue links of every
 * thread that's currently on it (kthread.next).  Other CPUs touch
 * this lock only when stealing work or migrating a thread; the
 * common case is uncontended.
 */
struct queue;	/* fwd; STREAMS queue_t -- see kappara/streams.h */

struct cpu {
	unsigned         cpu_id;
	struct kthread  *cpu_thread;	/* currently running on this CPU   */
	struct kthread  *cpu_idle;	/* this CPU's idle thread          */
	spinlock_t       cpu_disp_lock;
	struct kthread  *cpu_dispq_head;
	struct kthread  *cpu_dispq_tail;
	/* STREAMS service-procedure runqueue, drained from this CPU's
	 * sched_tick + sys_yield.  qenable on this CPU pushes here;
	 * the per-CPU drain is what gives us "natural" SVR4 streams
	 * scheduling on SMP. */
	struct queue    *cpu_srvq_head;
	struct queue    *cpu_srvq_tail;
	spinlock_t       cpu_srvq_lock;
};

#ifdef __aarch64__
static inline struct cpu *curcpu(void)
{
	struct cpu *c;
	__asm__ volatile ("mrs %0, tpidr_el1" : "=r"(c));
	return c;
}
static inline void set_curcpu(struct cpu *c)
{
	__asm__ volatile ("msr tpidr_el1, %0" :: "r"(c) : "memory");
}
#define curthread		(curcpu()->cpu_thread)
#define set_curthread(t)	do { curcpu()->cpu_thread = (t); } while (0)
#else
/* ARMv7 fallback: pretend there's one CPU, keep cur in a global. */
extern struct cpu  *_only_cpu;
static inline struct cpu *curcpu(void) { return _only_cpu; }
#define curthread		(curcpu()->cpu_thread)
#define set_curthread(t)	do { curcpu()->cpu_thread = (t); } while (0)
#endif

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

/* Return the kthread with the given tid, or NULL if no live thread
 * owns that id.  O(1) lookup via a sparse table; sys_kill uses this. */
struct kthread *kthread_find(unsigned tid);

/* Iteration helpers for /proc/ps -- the table is sparse so the
 * caller scans 0..max-1 and skips NULLs. */
unsigned        kthread_max_tid(void);
struct kthread *kthread_at(unsigned tid);

/* Human-readable name for a state.  Returns a small static string. */
const char     *kthread_state_name(enum kt_state s);

/* Set `sig` pending on `t`, and if `t` is parked on a wait queue
 * surgically remove it and put it on the ready queue so it wakes
 * and observes the signal.  Returns 0 on success or -1 if sig is
 * out of range. */
int             kthread_signal(struct kthread *t, unsigned sig);

/*
 * Arch-specific hook implemented in arch/<arch>/thread.c.  Lays
 * down the synthetic saved-register frame at stack_top so that
 * the first context_switch into the new thread pops fn/arg into
 * the callee-saved registers and "returns" into thread_trampoline.
 * Returns the SP value the thread should resume at.
 */
void *arch_thread_init_frame(void *stack_top, void (*fn)(void *), void *arg);

#endif

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

/* Forward decl: per-signal disposition, defined in signal.h.
 * We don't pull signal.h in here -- it'd be a circular include
 * (signal.h needs struct trap_frame -> trap.h, kthread is too
 * far up the chain to start dragging that). */
struct sigaction_k;

struct kthread {
	void          *sp;		/* saved kernel SP for context switch */
	void          *stack_base;	/* page allocated for the kernel stack */
	/* Display name for diagnostics (`/proc/ps`, kprintf).  Always
	 * points into the embedded `comm` field below -- kthread_create
	 * copies the caller's string in, so the source can be a stack
	 * buffer (e.g. sys_execve's resolved basename) without worry. */
	const char    *name;
	char           comm[32];
	/*
	 * Solaris-style polymorphic thread-state lock.
	 *
	 * t_lockp points at whichever spinlock currently owns this
	 * thread's mutable state (sp, state, waiting_on, next, queue
	 * linkage).  It is updated as the thread transitions between
	 * dispatch / sleep / per-CPU ownership:
	 *
	 *   newly created          -> &kthread.t_lock (per-thread default)
	 *   KT_RUNNING on a CPU    -> &cpu.cpu_thread_lock
	 *   KT_READY on a dispq    -> &cpu.cpu_disp_lock
	 *   KT_BLOCKED on a wait q -> &wait_queue.wq_lock  (phase 5)
	 *   KT_DEAD on to_reap     -> &to_reap_lock
	 *
	 * Future code mutates t state ONLY after acquiring *t_lockp,
	 * with retries when t_lockp changes mid-acquire (see
	 * thread_lock() in include/kappara/thread_lock.h, added in
	 * phase 1).  This is what closes the steal-mid-save race that
	 * the "/proc/ps text in a recycled stack page's saved-register
	 * slots" panic exposed.
	 *
	 * Phase 1 (this commit): the fields exist and are initialised
	 * but nothing reads t_lockp yet.  Phase 2 will wire swtch().
	 */
	spinlock_t    *t_lockp;
	spinlock_t     t_lock;
	unsigned       tid;
	enum kt_state  state;
	struct kthread *next;
	/* Pending-signal bitmap.  sys_kill sets bits, check_signals
	 * (from trap_dispatch's SVC return) consumes them.  See
	 * include/kappara/signal.h for the layout and SIGBIT(). */
	uint32_t       sig_pending;
	/* Currently-blocked signals.  pending & ~mask is what
	 * check_signals will actually deliver.  SIGKILL bypasses
	 * this mask -- enforced inside check_signals. */
	uint32_t       sig_mask;
	/* sigsuspend(2)'s "atomically restore on return" support.
	 * sigsuspend stashes the pre-call mask here and leaves
	 * sig_mask = the temporary mask while waiting.  sendsig
	 * uses sig_saved_mask (instead of sig_mask) when populating
	 * the sigframe so sigreturn unwinds to the right value;
	 * check_signals restores it directly if no handler ran.
	 * sig_mask_save_pending is the flag/guard. */
	uint32_t       sig_saved_mask;
	int            sig_mask_save_pending;
	/* Per-signal dispositions.  Indexed by signal number 1..NSIG-1
	 * (entry [0] is unused).  We embed the array inline so a
	 * sigaction() call is a one-field copy under no extra lock.
	 * sizeof(sigaction_k) is 16, NSIG is 32 -> 512 bytes per
	 * thread.  The size-1024 slab cache absorbs it. */
	struct sigaction_k *sig_actions;	/* allocated on first sigaction call */
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
	/*
	 * cpu_thread_lock owns the t_lockp of whichever kthread is
	 * currently KT_RUNNING on this CPU.  In phase 2 swtch() will
	 * hold it across context_switch -- the OUTGOING thread's
	 * dispatcher acquires it (it equals outgoing->t_lockp at
	 * entry); the INCOMING thread's continuation releases it
	 * (it equals incoming->t_lockp after the transfer inside
	 * swtch).  That's the structural close for the
	 * steal-mid-save race.  In phase 1 the lock exists and is
	 * initialised but nothing uses it yet.
	 */
	spinlock_t       cpu_thread_lock;
	/*
	 * Pending requeue stash for swtch's deferred dispq push.
	 * The OUTGOING thread sets this BEFORE context_switch; the
	 * INCOMING thread (or thread_trampoline) reads it AFTER
	 * context_switch and pushes the stashed thread onto the
	 * dispq with its t_lockp transitioned to cpu_disp_lock.
	 * Deferring the push to the after-switch side is what closes
	 * the steal-mid-save race -- a stealer that gets the stashed
	 * thread reads a sp that has been committed by
	 * context_switch's save phase.
	 */
	struct kthread  *cpu_pending_requeue;
	/*
	 * Optional second lock the resumer releases after
	 * context_switch.  When the OUTGOING thread held a lock across
	 * the switch in addition to cpu_thread_lock (specifically:
	 * sleepers hold wq->sq_lock to keep wakers from observing the
	 * outgoing thread's stale sp during the save phase), it
	 * stashes the pointer here so the INCOMING continuation
	 * releases it.  NULL means "no extra lock" (the yield case).
	 */
	spinlock_t      *cpu_pending_release_lock;
	struct kthread  *cpu_dispq_head;
	struct kthread  *cpu_dispq_tail;
	/* Maintained alongside head/tail by every push/pop on the
	 * dispatch queue.  The push-side load balancer (`dispq_push`)
	 * reads this WITHOUT taking the remote CPU's lock -- a stale
	 * value just means we pick a slightly-suboptimal target, the
	 * subsequent locked push/pop is correct either way.  Lockless
	 * reads of a single word are atomic on AArch64. */
	unsigned         cpu_dispq_len;
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

/* Per-secondary-CPU initialisation.  Called once per secondary core
 * after that core has its MMU on and VBAR_EL1 installed.  Allocates
 * the per-CPU idle kthread, writes TPIDR_EL1, and makes curcpu() /
 * curthread valid on that core. */
void            sched_secondary_init(unsigned cpu_id);

/* Diagnostic snapshot of a single CPU's dispatcher state -- consumed
 * by /proc/cpuload.  Cheap to fill (single-word reads, no locks).
 * Stale-by-an-instant by design: this is observation, not control. */
struct sched_cpu_info {
	unsigned     cpu_id;
	unsigned     dispq_len;
	int          idle;		/* 1 if this CPU is in its idle thread */
	const char  *cur_name;		/* curthread->name on this CPU         */
};

unsigned        sched_ncpu(void);
void            sched_get_cpu_info(unsigned i, struct sched_cpu_info *out);

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
 * SMP discipline (Solaris sleepq).  sq_lock guards the queue body
 * (wq->head + the linkage of every thread currently on it) AND, more
 * importantly, is held by the sleeper across context_switch's save
 * phase.  The mechanism: kthread_sleep_on acquires sq_lock, links
 * self onto wq->head, then passes &sq_lock to switch_to_next as
 * extra_release.  switch_to_next stashes it in cpu_pending_release_lock;
 * the INCOMING thread releases sq_lock AFTER context_switch returns.
 * A waker therefore cannot acquire sq_lock until the sleeper's sp has
 * been committed -- which closes the wake-side variant of the
 * steal-mid-save race that Phase 2 closed for the yield path.
 */
struct wait_queue {
	struct kthread *head;
	spinlock_t      sq_lock;
};

#define WAIT_QUEUE_INIT		{ .head = NULL, .sq_lock = SPINLOCK_INIT }

void            kthread_sleep_on (struct wait_queue *wq);

/*
 * Atomic check-and-sleep variant.  Caller MUST already hold wq->sq_lock
 * (taken via spin_lock_irq_save, returning flags it captured) and pass
 * the captured flags in.  This routine links the caller onto wq and
 * yields without re-queuing -- the sq_lock release happens AFTER
 * context_switch commits sp (via extra_release), same as the regular
 * sleep path.  On return, sq_lock is NOT held and DAIF has been
 * restored to `flags`.
 *
 * The point: callers with a "while (!condition) sleep" pattern (sys_wait,
 * sys_sigsuspend, stream_read) can take sq_lock, evaluate `condition`
 * under it, and -- if they need to sleep -- hand off to this routine.
 * A waker on another CPU that sets the condition under sq_lock will
 * either:
 *   (a) get sq_lock first, set the condition, release.  The caller's
 *       subsequent acquire sees the new condition, no sleep.
 *   (b) spin on sq_lock until the caller has slept (release happens
 *       after ctx_switch save).  Then sets condition and wakes.
 * Either way, no lost wakeup.
 */
void            kthread_sleep_on_locked(struct wait_queue *wq,
					unsigned long flags);
void            kthread_wake_all (struct wait_queue *wq);
void            kthread_wake_one (struct wait_queue *wq);

/* Broadcast wait queue woken by every kthread_exit -- sys_wait sleeps
 * on it and re-checks the target tid on wake. */
extern struct wait_queue thread_exit_wq;

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

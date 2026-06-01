/*
 * kernel/sched.c -- round-robin kernel-thread scheduler
 * =====================================================
 *
 * What this file is
 * -----------------
 * The portable scheduler.  Knows about threads, the ready queue, and
 * cooperative + preemptive yields.  Knows nothing about timers,
 * interrupts, or CPU-specific register layouts -- that all lives in
 * the arch tree (timer driver + context_switch in arch/aarch64/).
 *
 * One ready queue, FIFO, no priorities (yet)
 * ------------------------------------------
 *
 *     ready_head -> A -> B -> C -> NULL <- ready_tail
 *
 *     cur = M   (whoever's currently running)
 *
 *     M calls kthread_yield():
 *        next = ready_pop()      ; next = A,   ready = B->C->NULL
 *        ready_push(M)           ; ready = B->C->M->NULL
 *        cur = A
 *        context_switch(&M->sp, A->sp)
 *
 *     A runs.  Eventually it yields and round-robin continues:
 *
 *        B->C->M->A->B->C->M->A->...
 *
 * Per-thread state
 * ----------------
 *   struct kthread {
 *       void *sp;          // saved kernel SP, the resume point
 *       void *stack_base;  // page allocated for this thread's stack
 *       const char *name;  // for diagnostics
 *       unsigned tid;
 *       enum   state;      // READY / RUNNING / BLOCKED
 *       struct kthread *next;  // ready-queue link
 *   };
 *
 * Each thread gets one 4 KB page (carved from pmm) for its kernel
 * stack.  That's also where the trap frame lands when an IRQ
 * preempts the thread, so 4 KB is enough as long as we keep the
 * stack-using code lean.
 *
 * Cooperative + preemptive in the same call
 * -----------------------------------------
 * kthread_yield() works whether you call it directly from C or whether
 * it's invoked from the timer-IRQ handler (via sched_tick).  The trick
 * is that context_switch in switch.S only touches callee-saved
 * registers; whatever was on the stack above the switch frame -- a
 * normal C call stack, OR a trap frame from KERNEL_ENTRY -- is
 * preserved untouched on the outgoing thread's stack.  When that
 * thread is later picked again, it resumes back through whatever path
 * brought it in.
 *
 * Thread bootstrap
 * ----------------
 * The first time the scheduler picks a new thread, context_switch
 * pops a synthetic stack frame that kthread_create() laid down:
 *
 *     stack top  ---->  +-----------+
 *                       |  x29 = 0  |
 *                       |  x30 =    | <- thread_trampoline
 *                       +-----------+
 *                       |  x27, x28 | (zeros)
 *                       +-----------+
 *                       |  x25, x26 |
 *                       +-----------+
 *                       |  x23, x24 |
 *                       +-----------+
 *                       |  x21, x22 |
 *                       +-----------+
 *     saved SP -------> |  x19 = fn |
 *                       |  x20 = arg|
 *                       +-----------+
 *
 * After context_switch's ldp's, x19 holds fn, x20 holds arg, and the
 * `ret` jumps to thread_trampoline (arch/aarch64/switch.S) which
 * unmasks IRQs and tail-calls fn(arg).
 */

#include <stdint.h>

#include "kappara/kmem.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/signal.h"
#include "kappara/streams.h"
#include "kappara/sched.h"
#include "kappara/string.h"
#include "kappara/vfs.h"

extern void context_switch(void **save_sp, void *new_sp);

/*
 * Per-CPU dispatcher state.  One slot per core; cpu_id is the
 * index.  Core 0's slot is set up in sched_init; secondary cores
 * (next commit) initialize their own and write TPIDR_EL1 at
 * bring-up time.
 *
 * This is the SVR4 dispatcher shape: each CPU has its own runqueue
 * + lock, so the common case (this CPU schedules its own threads)
 * touches no shared state.  Cross-CPU traffic (idle steal, wake-
 * other-cpu) is bounded and goes through the target's lock.
 */
#define KSCHED_NCPU	4
static struct cpu cpus[KSCHED_NCPU];

#ifndef __aarch64__
struct cpu *_only_cpu = &cpus[0];
#endif

static unsigned next_tid = 1;

/*
 * Threads that called kthread_exit but haven't been reaped yet.
 * Stays global -- any CPU can reap any dying thread, the only rule
 * is "don't reap while standing on the dying thread's stack."
 * to_reap_lock guards the list.
 */
static struct kthread *to_reap;
static spinlock_t      to_reap_lock = SPINLOCK_INIT;

/*
 * Sparse table from tid -> kthread *.  next_tid increments by 1
 * each kthread_create; we don't recycle slots when threads die, so
 * tid values are unique within a boot and grow monotonically.  Cap
 * is small for now since spawn pool is also small (sub-32 typical).
 */
#define KSCHED_MAX_TID	256
static struct kthread *tid_table[KSCHED_MAX_TID];
static spinlock_t      tid_lock = SPINLOCK_INIT;

/* Append t to CPU c's dispatch queue.  Caller MUST hold c->cpu_disp_lock. */
static void dispq_push_locked(struct cpu *c, struct kthread *t)
{
	t->next = NULL;
	if (c->cpu_dispq_tail)
		c->cpu_dispq_tail->next = t;
	else
		c->cpu_dispq_head = t;
	c->cpu_dispq_tail = t;
}

/* Pop the head of CPU c's dispatch queue.  Caller MUST hold the lock. */
static struct kthread *dispq_pop_locked(struct cpu *c)
{
	struct kthread *t = c->cpu_dispq_head;
	if (t) {
		c->cpu_dispq_head = t->next;
		if (!c->cpu_dispq_head)
			c->cpu_dispq_tail = NULL;
		t->next = NULL;
	}
	return t;
}

/* Lock + push (used by wake paths from outside switch_to_next). */
static void dispq_push(struct cpu *c, struct kthread *t)
{
	unsigned long f = spin_lock_irq_save(&c->cpu_disp_lock);
	dispq_push_locked(c, t);
	spin_unlock_irq_restore(&c->cpu_disp_lock, f);
}

void sched_init(void)
{
	static struct kthread main_thread;
	main_thread.name  = "main";
	main_thread.tid   = 0;
	main_thread.state = KT_RUNNING;
	main_thread.next  = NULL;

	/* Initialize core 0's per-CPU struct and publish via TPIDR_EL1
	 * before anything touches curcpu/curthread. */
	cpus[0].cpu_id        = 0;
	cpus[0].cpu_thread    = &main_thread;
	cpus[0].cpu_idle      = &main_thread;	/* main IS our idle */
	set_curcpu(&cpus[0]);

	tid_table[0] = &main_thread;
	kprintf("sched: cpu 0 main thread is tid=0\n");
}

struct kthread *kthread_create(const char *name, void (*fn)(void *), void *arg)
{
	struct kthread *t = kmalloc(sizeof(*t));
	if (!t)
		return NULL;
	/* Slab returns recycled memory; zero the whole struct so any
	 * field we don't explicitly assign (fdt[], future signal
	 * fields, ...) starts clean instead of holding stale bytes
	 * from a previously freed thread. */
	kmemset(t, 0, sizeof(*t));
	void *stack = pmm_alloc();
	if (!stack) {
		kfree(t);
		return NULL;
	}

	t->name       = name;
	{
		unsigned long f = spin_lock_irq_save(&tid_lock);
		t->tid = next_tid++;
		if (t->tid < KSCHED_MAX_TID)
			tid_table[t->tid] = t;
		spin_unlock_irq_restore(&tid_lock, f);
	}
	t->stack_base = stack;
	t->state      = KT_READY;
	t->next       = NULL;
	t->sp         = arch_thread_init_frame((char *)stack + PAGE_SIZE,
					       fn, arg);

	/* For now, every new thread lands on the creator's CPU.
	 * Cross-CPU load balancing (idle steal) is a separate step. */
	dispq_push(curcpu(), t);
	kprintf("sched: created tid=%u name=%s stack=%p fn=%p\n",
		t->tid, name, stack, (void *)(uintptr_t)fn);
	return t;
}

/* Common scheduler step.  If requeue_current is set, the outgoing
 * thread goes back on THIS CPU's dispatch queue (cooperative yield);
 * if not, it has already been parked somewhere else (a wait queue,
 * exit limbo, ...) and we MUST NOT put it back.
 *
 * The lock dance: take cpu_disp_lock for the queue mutations and
 * the cur=next swap, release before the actual context_switch so
 * the incoming thread doesn't inherit a held lock.  IRQs stay
 * masked across the switch since context_switch saves/restores
 * per-thread DAIF (commit 0929814). */
static void switch_to_next(int requeue_current)
{
	struct cpu *c = curcpu();
	unsigned long flags = spin_lock_irq_save(&c->cpu_disp_lock);

	struct kthread *next = dispq_pop_locked(c);
	if (!next) {
		spin_unlock_irq_restore(&c->cpu_disp_lock, flags);
		if (requeue_current)
			return;
		/* Nobody runnable AND the caller is about to block.
		 * That's a deadlock for now (no idle thread of our
		 * own).  Park on WFE so an IRQ can wake us into a
		 * wake path. */
		kprintf("sched: cpu %u nothing to run; deadlock\n",
			c->cpu_id);
		for (;;)
			__asm__ volatile ("wfe");
	}

	struct kthread *prev = c->cpu_thread;
	if (requeue_current) {
		dispq_push_locked(c, prev);
		prev->state = KT_READY;
	}
	next->state = KT_RUNNING;
	c->cpu_thread = next;
	/* Release the lock; keep IRQs masked across the switch. */
	spin_unlock(&c->cpu_disp_lock);

	context_switch(&prev->sp, next->sp);

	/* We're now running as `next`.  Drain the to-reap list: any
	 * thread that called kthread_exit added itself here and we're
	 * guaranteed to be on a DIFFERENT stack now, so freeing is safe. */
	unsigned long f2 = spin_lock_irq_save(&to_reap_lock);
	struct kthread *grab = to_reap;
	to_reap = NULL;
	spin_unlock_irq_restore(&to_reap_lock, f2);
	while (grab) {
		struct kthread *t = grab;
		grab = t->next;
		if (t->stack_base) {
			if (t->tid < KSCHED_MAX_TID) {
				unsigned long f3 = spin_lock_irq_save(&tid_lock);
				tid_table[t->tid] = NULL;
				spin_unlock_irq_restore(&tid_lock, f3);
			}
			pmm_free(t->stack_base);
			kfree(t);
		}
	}

	/* Restore the IRQ state from when we entered switch_to_next. */
	__asm__ volatile ("msr daif, %0" :: "r"(flags) : "memory");
}

void kthread_yield(void)
{
	switch_to_next(1);
}

/* Bracket the wait-queue mutation + context switch so the timer
 * tick can't fire between marking us BLOCKED and actually leaving
 * the CPU -- otherwise we'd be on a wait queue AND on the ready
 * queue, and the next scheduling decision would run us with
 * state=BLOCKED.  IRQs are restored on the wake side by the new
 * thread's own resume path. */
static inline unsigned long irq_save_and_disable(void)
{
	unsigned long daif;
	__asm__ volatile ("mrs %0, daif" : "=r"(daif));
	__asm__ volatile ("msr daifset, #2" ::: "memory");
	return daif;
}

static inline void irq_restore(unsigned long daif)
{
	__asm__ volatile ("msr daif, %0" :: "r"(daif) : "memory");
}

void kthread_sleep_on(struct wait_queue *wq)
{
	unsigned long flags = irq_save_and_disable();
	struct kthread *t = curthread;
	t->state      = KT_BLOCKED;
	t->waiting_on = wq;
	t->next       = wq->head;
	wq->head      = t;
	switch_to_next(0);
	/* Reached here after someone called kthread_wake_*; the new
	 * DAIF that context_switch restored is whatever this thread
	 * had at the moment it slept (masked) -- so restore the
	 * pre-sleep state explicitly to put us back at the caller's
	 * IRQ-mask level. */
	curthread->waiting_on = NULL;
	irq_restore(flags);
}

void kthread_wake_all(struct wait_queue *wq)
{
	unsigned long flags = irq_save_and_disable();
	struct kthread *t = wq->head;
	wq->head = NULL;
	irq_restore(flags);
	/* Wake each onto the WAKER's CPU.  Cross-CPU scheduling will
	 * pick it up via idle steal later; for now, locality. */
	struct cpu *c = curcpu();
	while (t) {
		struct kthread *n = t->next;
		t->next       = NULL;
		t->waiting_on = NULL;
		t->state      = KT_READY;
		dispq_push(c, t);
		t = n;
	}
}

void kthread_wake_one(struct wait_queue *wq)
{
	unsigned long flags = irq_save_and_disable();
	struct kthread *t = wq->head;
	if (t) {
		wq->head      = t->next;
		t->next       = NULL;
		t->waiting_on = NULL;
		t->state      = KT_READY;
	}
	irq_restore(flags);
	if (t)
		dispq_push(curcpu(), t);
}

struct kthread *kthread_find(unsigned tid)
{
	if (tid >= KSCHED_MAX_TID) return NULL;
	unsigned long f = spin_lock_irq_save(&tid_lock);
	struct kthread *t = tid_table[tid];
	spin_unlock_irq_restore(&tid_lock, f);
	if (t && t->state == KT_DEAD) return NULL;
	return t;
}

unsigned kthread_max_tid(void)
{
	return KSCHED_MAX_TID;
}

struct kthread *kthread_at(unsigned tid)
{
	if (tid >= KSCHED_MAX_TID) return NULL;
	return tid_table[tid];	/* includes DEAD; /proc/ps wants to see them */
}

const char *kthread_state_name(enum kt_state s)
{
	switch (s) {
	case KT_READY:   return "READY";
	case KT_RUNNING: return "RUN";
	case KT_BLOCKED: return "BLOCK";
	case KT_DEAD:    return "DEAD";
	}
	return "?";
}

int kthread_signal(struct kthread *t, unsigned sig)
{
	if (!t || sig == 0 || sig >= NSIG) return -1;
	int needs_dispq_push = 0;
	unsigned long flags = irq_save_and_disable();
	t->sig_pending |= SIGBIT(sig);
	/* If t is sleeping on a wait queue, surgically unlink it and
	 * mark it READY so it wakes and observes the signal.  The
	 * blocking primitive is expected to re-check sig_pending
	 * after sleep_on returns. */
	if (t->state == KT_BLOCKED && t->waiting_on) {
		struct wait_queue *wq = t->waiting_on;
		if (wq->head == t) {
			wq->head = t->next;
		} else {
			for (struct kthread *p = wq->head; p; p = p->next) {
				if (p->next == t) {
					p->next = t->next;
					break;
				}
			}
		}
		t->next       = NULL;
		t->waiting_on = NULL;
		t->state      = KT_READY;
		needs_dispq_push = 1;
	}
	irq_restore(flags);
	if (needs_dispq_push)
		dispq_push(curcpu(), t);
	return 0;
}

void sched_tick(void)
{
	/* Each tick: drain any deferred STREAMS work, then rotate. */
	streams_run();
	kthread_yield();
}

void kthread_exit(void)
{
	struct kthread *me = curthread;
	/* Release every file the dying thread still holds.  pipe ends
	 * inherited from the parent get their refcount dropped here so
	 * the pipe goes away naturally when both ends are closed. */
	vfs_drain_fds(me);
	kprintf("kthread: tid=%u (%s) exited\n", me->tid, me->name);

	/* Park ourselves on the to-reap list -- some other thread will
	 * free our stack + kthread once we've context-switched away. */
	unsigned long flags = spin_lock_irq_save(&to_reap_lock);
	me->state = KT_DEAD;
	me->next  = to_reap;
	to_reap   = me;
	spin_unlock_irq_restore(&to_reap_lock, flags);
	switch_to_next(0);
	/* unreachable: switch_to_next with requeue=0 never returns
	 * once cur is no longer in the ready queue and never woken. */
	(void)flags;
	for (;;)
		__asm__ volatile ("wfe");
}

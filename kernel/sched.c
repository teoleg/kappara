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
#include "kappara/streams.h"
#include "kappara/sched.h"
#include "kappara/vfs.h"

extern void context_switch(void **save_sp, void *new_sp);

struct kthread *cur;

static struct kthread *ready_head;
static struct kthread *ready_tail;
static unsigned        next_tid = 1;

/*
 * Threads that called kthread_exit but haven't been reaped yet.
 * A thread can't free its own kernel stack while running on it; it
 * adds itself here and yields.  The very next switch_to_next call
 * runs after we've switched OFF the dying thread, on the new
 * thread's stack -- safe to free the body now.  Single linked list
 * via kthread.next, same as the ready queue (the two are mutually
 * exclusive: dead threads aren't ready).
 */
static struct kthread *to_reap;

static void ready_push(struct kthread *t)
{
	t->next = NULL;
	if (ready_tail)
		ready_tail->next = t;
	else
		ready_head = t;
	ready_tail = t;
}

static struct kthread *ready_pop(void)
{
	struct kthread *t = ready_head;
	if (t) {
		ready_head = t->next;
		if (!ready_head)
			ready_tail = NULL;
		t->next = NULL;
	}
	return t;
}

void sched_init(void)
{
	static struct kthread main_thread;
	main_thread.name  = "main";
	main_thread.tid   = 0;
	main_thread.state = KT_RUNNING;
	main_thread.next  = NULL;
	cur = &main_thread;
	kprintf("sched: main thread is tid=0\n");
}

struct kthread *kthread_create(const char *name, void (*fn)(void *), void *arg)
{
	struct kthread *t = kmalloc(sizeof(*t));
	if (!t)
		return NULL;
	void *stack = pmm_alloc();
	if (!stack) {
		kfree(t);
		return NULL;
	}

	t->name       = name;
	t->tid        = next_tid++;
	t->stack_base = stack;
	t->state      = KT_READY;
	t->next       = NULL;
	t->sp         = arch_thread_init_frame((char *)stack + PAGE_SIZE,
					       fn, arg);

	ready_push(t);
	kprintf("sched: created tid=%u name=%s stack=%p fn=%p\n",
		t->tid, name, stack, (void *)(uintptr_t)fn);
	return t;
}

/* Common scheduler step.  If requeue_current is set, the outgoing
 * thread goes back on the ready queue (cooperative yield); if not,
 * it has already been parked somewhere else (a wait queue, exit
 * limbo, ...) and we MUST NOT put it back on the ready queue or
 * the scheduler will think it's runnable. */
static void switch_to_next(int requeue_current)
{
	struct kthread *next = ready_pop();
	if (!next) {
		if (requeue_current)
			return;
		/* Nobody runnable AND the caller is about to block.
		 * That's a deadlock for now (no idle thread).  Park
		 * on WFE so an IRQ -- specifically the timer or a
		 * future device interrupt -- can wake us into a wake
		 * path that puts somebody on the ready queue. */
		kprintf("sched: nothing to run; deadlock or pure idle\n");
		for (;;)
			__asm__ volatile ("wfe");
	}
	struct kthread *prev = cur;
	if (requeue_current) {
		ready_push(prev);
		prev->state = KT_READY;
	}
	next->state = KT_RUNNING;
	cur = next;
	context_switch(&prev->sp, next->sp);

	/* We're now running as `next` (cur = next, but that's the same
	 * pointer the OLD context had).  Drain the to-reap list: any
	 * thread that called kthread_exit added itself here and we're
	 * guaranteed to be on a DIFFERENT stack now, so freeing those
	 * pages is safe.  The main thread is a static struct with no
	 * pmm-allocated stack, so we use stack_base != NULL as the
	 * "safe to free" marker. */
	while (to_reap) {
		struct kthread *t = to_reap;
		to_reap = t->next;
		if (t->stack_base) {
			pmm_free(t->stack_base);
			kfree(t);
		}
	}
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
	cur->state = KT_BLOCKED;
	cur->next  = wq->head;
	wq->head   = cur;
	switch_to_next(0);
	/* Reached here after someone called kthread_wake_*; the new
	 * DAIF that context_switch restored is whatever this thread
	 * had at the moment it slept (masked) -- so restore the
	 * pre-sleep state explicitly to put us back at the caller's
	 * IRQ-mask level. */
	irq_restore(flags);
}

void kthread_wake_all(struct wait_queue *wq)
{
	unsigned long flags = irq_save_and_disable();
	struct kthread *t = wq->head;
	wq->head = NULL;
	while (t) {
		struct kthread *n = t->next;
		t->next  = NULL;
		t->state = KT_READY;
		ready_push(t);
		t = n;
	}
	irq_restore(flags);
}

void kthread_wake_one(struct wait_queue *wq)
{
	unsigned long flags = irq_save_and_disable();
	struct kthread *t = wq->head;
	if (t) {
		wq->head = t->next;
		t->next  = NULL;
		t->state = KT_READY;
		ready_push(t);
	}
	irq_restore(flags);
}

void sched_tick(void)
{
	/* Each tick: drain any deferred STREAMS work, then rotate. */
	streams_run();
	kthread_yield();
}

void kthread_exit(void)
{
	/* Release every file the dying thread still holds.  pipe ends
	 * inherited from the parent get their refcount dropped here so
	 * the pipe goes away naturally when both ends are closed. */
	vfs_drain_fds(cur);
	kprintf("kthread: tid=%u (%s) exited\n", cur->tid, cur->name);

	/* Park ourselves on the to-reap list -- some other thread will
	 * free our stack + kthread once we've context-switched away. */
	unsigned long flags = irq_save_and_disable();
	cur->state = KT_DEAD;
	cur->next  = to_reap;
	to_reap    = cur;
	switch_to_next(0);
	/* unreachable: switch_to_next with requeue=0 never returns
	 * once cur is no longer in the ready queue and never woken. */
	(void)flags;
	for (;;)
		__asm__ volatile ("wfe");
}

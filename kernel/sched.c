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
#include "kappara/sched.h"

extern void context_switch(void **save_sp, void *new_sp);

struct kthread *cur;

static struct kthread *ready_head;
static struct kthread *ready_tail;
static unsigned        next_tid = 1;

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

void kthread_yield(void)
{
	struct kthread *next = ready_pop();
	if (!next)
		return;
	struct kthread *prev = cur;
	ready_push(prev);
	prev->state = KT_READY;
	next->state = KT_RUNNING;
	cur = next;
	context_switch(&prev->sp, next->sp);
}

void sched_tick(void)
{
	kthread_yield();
}

void kthread_exit(void)
{
	kprintf("kthread: tid=%u (%s) exited\n", cur->tid, cur->name);
	for (;;)
		__asm__ volatile ("wfe");
}

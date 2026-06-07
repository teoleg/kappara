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

#include "kappara/ipi.h"
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
 * Sentinel for `kthread.t_lockp` mid-transition (phase 1: declared,
 * not yet used).  See include/kappara/thread_lock.h for the role.
 * No code spin_locks this; it's just a uniquely-identifiable
 * non-NULL pointer.
 */
spinlock_t t_transition_lock = SPINLOCK_INIT;

/* Global wait queue for sys_wait().  Every kthread_exit wakes
 * everyone on it; each waiter re-checks whether ITS target tid is
 * now gone.  A single broadcast is fine -- the cost is one yield
 * per waiter per peer-exit, and waitpid storms aren't a hot path. */
struct wait_queue thread_exit_wq;

/*
 * Bitmask of CPUs currently running their idle thread.  Wakers use
 * ipi_wake_idle() to nudge idle CPUs to come look for work without
 * waiting up to one tick for them to notice on their own.  Plain
 * read/write -- a stale bit just costs one wasted IPI or one missed
 * IPI, neither of which is a correctness issue (the next tick covers
 * both).
 */
static volatile uint32_t cpu_idle_mask;

uint32_t sched_idle_mask(void)
{
	return cpu_idle_mask;
}

unsigned sched_ncpu(void)
{
	return KSCHED_NCPU;
}

void sched_get_cpu_info(unsigned i, struct sched_cpu_info *out)
{
	if (!out) return;
	if (i >= KSCHED_NCPU) {
		out->cpu_id    = 0;
		out->dispq_len = 0;
		out->idle      = 0;
		out->cur_name  = "?";
		return;
	}
	/* All single-word reads; the snapshot may be stale by the time
	 * the caller formats it, which is fine for a diagnostic dump.
	 * No lock is taken so /proc/cpuload doesn't contend with the
	 * scheduler hot path. */
	struct cpu *c = &cpus[i];
	out->cpu_id    = c->cpu_id;
	out->dispq_len = c->cpu_dispq_len;
	out->idle      = (cpu_idle_mask >> i) & 1u;
	out->cur_name  = (c->cpu_thread && c->cpu_thread->name)
			 ? c->cpu_thread->name
			 : "?";
}

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
	c->cpu_dispq_len++;
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
		c->cpu_dispq_len--;
	}
	return t;
}

/*
 * Pick a target CPU for a newly-runnable thread.
 *
 * Default policy until now: always queue on the caller's CPU and let
 * idle steal redistribute.  Pull-side only.  That works for the
 * common case where one CPU is keeping up but degrades badly when a
 * burst of wakeups (e.g. timer expiries hitting many sleeping
 * threads at once) all hit the same source CPU faster than the
 * idlers can steal them -- the source's dispq grows long while
 * other CPUs sit empty for a tick.
 *
 * Push-side selection: prefer an idle CPU if there is one (idle
 * mask is a single volatile word, cheap to read); otherwise pick
 * the CPU with the shortest dispatch queue.  Ties go to the caller
 * so steady-state behaviour matches the old policy when load is
 * balanced.  All reads of cpu_dispq_len are unlocked -- a stale
 * value just costs us a slightly worse pick, the subsequent locked
 * push lands on whatever CPU we settled on.
 *
 * The threshold (`>= 2 local-queued threads`) avoids spreading
 * single short-running work items across cores -- a one-off wake
 * stays on the waker's CPU for cache locality.  Only when there's
 * a real pile-up do we hand work off.
 */
static struct cpu *pick_push_target(struct cpu *caller)
{
	/* Path 1: any CPU idle?  ipi_wake_idle handles the wake. */
	uint32_t idle = cpu_idle_mask & ~(1u << caller->cpu_id);
	if (idle) {
		for (unsigned i = 0; i < KSCHED_NCPU; i++) {
			if (idle & (1u << i))
				return &cpus[i];
		}
	}

	/* Path 2: every CPU busy.  Hand the new thread to the
	 * least-loaded CPU only if our own queue is non-trivial --
	 * otherwise keep it local for cache locality. */
	if (caller->cpu_dispq_len < 2)
		return caller;

	struct cpu *best   = caller;
	unsigned    best_n = caller->cpu_dispq_len;
	for (unsigned i = 0; i < KSCHED_NCPU; i++) {
		struct cpu *v = &cpus[i];
		if (v == caller) continue;
		unsigned n = v->cpu_dispq_len;
		if (n + 1 < best_n) {	/* strict: avoid bouncing on ties */
			best   = v;
			best_n = n;
		}
	}
	return best;
}

/* Lock + push (used by wake paths from outside switch_to_next).
 *
 * Push-side load balancing: instead of always queuing on `c` (the
 * caller's CPU), pick_push_target may redirect to an idle peer or
 * a less-loaded peer.  After pushing, IPI any idle CPUs so the
 * pick reaches WFI'd cores immediately.
 */
static void dispq_push(struct cpu *c, struct kthread *t)
{
	struct cpu *tgt = pick_push_target(c);
	unsigned long f = spin_lock_irq_save(&tgt->cpu_disp_lock);
	dispq_push_locked(tgt, t);
	spin_unlock_irq_restore(&tgt->cpu_disp_lock, f);
	if (cpu_idle_mask)
		ipi_wake_idle();
}

void sched_init(void)
{
	static struct kthread main_thread;
	main_thread.name  = "main";
	main_thread.tid   = 0;
	main_thread.state = KT_RUNNING;
	main_thread.next  = NULL;

	/* Initialize core 0's per-CPU struct and publish via TPIDR_EL1
	 * before anything touches curcpu/curthread.  Spinlocks live
	 * in the bss-zeroed `cpus` array so they start unlocked. */
	cpus[0].cpu_id        = 0;
	cpus[0].cpu_thread    = &main_thread;
	cpus[0].cpu_idle      = &main_thread;	/* main IS our idle */
	set_curcpu(&cpus[0]);

	/*
	 * Polymorphic state lock: main_thread is KT_RUNNING on cpu 0,
	 * so its t_lockp is cpu 0's cpu_thread_lock per the discipline
	 * documented on struct kthread::t_lockp.  Phase 1 sets the
	 * pointer for future readers; nothing reads it yet.
	 */
	main_thread.t_lockp   = &cpus[0].cpu_thread_lock;

	/* Core 0 starts on its idle (== main) thread.  Mark it so. */
	cpu_idle_mask |= (1u << 0);

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

	/* Copy `name` into t->comm so the caller can pass a string with
	 * any lifetime (a stack-allocated basename from sys_execve, a
	 * .rodata literal, ...) and t->name stays valid for as long as
	 * the kthread does.  Bounded to comm[31] + NUL. */
	if (name) {
		size_t i = 0;
		while (name[i] && i + 1 < sizeof(t->comm)) {
			t->comm[i] = name[i];
			i++;
		}
		t->comm[i] = '\0';
	}
	t->name       = t->comm;
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
	/* Initial polymorphic state lock: the per-thread default.
	 * dispq_push() will transition t_lockp to the target CPU's
	 * cpu_disp_lock in a later phase; for now this is just a
	 * stable initial value so nothing reads NULL. */
	t->t_lockp    = &t->t_lock;
	t->sp         = arch_thread_init_frame((char *)stack + PAGE_SIZE,
					       fn, arg);

	/* For now, every new thread lands on the creator's CPU.
	 * Cross-CPU load balancing (idle steal) is a separate step. */
	dispq_push(curcpu(), t);
	kprintf("sched: created tid=%u name=%s stack=%p fn=%p\n",
		t->tid, name, stack, (void *)(uintptr_t)fn);
	return t;
}

/*
 * Idle steal: scan other CPUs' dispatch queues and pull one thread.
 * Called with IRQs already masked (we only reach here after
 * spin_lock_irq_save + spin_unlock on our own lock, so DAIF is still
 * set).  spin_lock_irq_save on the victim lock is idempotent when IRQs
 * are already masked -- save captures the masked state, restore keeps
 * it.
 *
 * Quick lockless peek at cpu_dispq_head before acquiring the victim's
 * lock; a stale NULL just costs one missed steal, which is harmless.
 */
static struct kthread *try_steal(struct cpu *my_cpu)
{
	for (unsigned i = 0; i < KSCHED_NCPU; i++) {
		struct cpu *v = &cpus[i];
		if (v == my_cpu)            continue;
		if (!v->cpu_dispq_head)     continue;  /* racy but harmless */
		unsigned long f = spin_lock_irq_save(&v->cpu_disp_lock);
		struct kthread *t = dispq_pop_locked(v);
		spin_unlock_irq_restore(&v->cpu_disp_lock, f);
		if (t) return t;
	}
	return NULL;
}

/* Common scheduler step.  If requeue_current is set, the outgoing
 * thread goes back on THIS CPU's dispatch queue (cooperative yield);
 * if not, it has already been parked somewhere else (a wait queue,
 * exit limbo, ...) and we MUST NOT put it back.
 *
 * Lock discipline: spin_lock_irq_save at entry masks IRQs for the
 * entire switch; spin_unlock releases the queue lock but KEEPS IRQs
 * masked until the end.  When the local queue is empty we release the
 * queue lock (keeping IRQs masked) and try to steal from another CPU.
 * If that also turns up nothing we fall through to the per-CPU idle
 * thread so the CPU can WFI rather than spin.
 */
static void switch_to_next(int requeue_current)
{
	struct cpu *c = curcpu();
	unsigned long flags = spin_lock_irq_save(&c->cpu_disp_lock);

	struct kthread *next = dispq_pop_locked(c);
	if (!next) {
		/* Release queue lock; keep IRQs masked for try_steal. */
		spin_unlock(&c->cpu_disp_lock);
		next = try_steal(c);
		if (!next) {
			if (requeue_current) {
				/* Yield with nothing to do: keep running. */
				__asm__ volatile ("msr daif, %0"
						  :: "r"(flags) : "memory");
				return;
			}
			/* Thread is blocking or exiting.  Run the CPU's
			 * idle thread so the core can WFI rather than
			 * burning cycles. */
			next = c->cpu_idle;
		}
		if (!next) {
			kprintf("sched: cpu %u nothing to run; deadlock\n",
				c->cpu_id);
			for (;;)
				__asm__ volatile ("wfe");
		}
		/* Re-acquire the lock for the thread swap below. */
		spin_lock(&c->cpu_disp_lock);
	}

	struct kthread *prev = c->cpu_thread;
	if (next == prev) {
		/* Idle→idle or steal returned current thread: no-op. */
		spin_unlock(&c->cpu_disp_lock);
		__asm__ volatile ("msr daif, %0" :: "r"(flags) : "memory");
		return;
	}
	if (requeue_current && prev != c->cpu_idle) {
		/* The per-CPU idle thread is a singleton fallback, not a
		 * runqueue citizen.  When idle yields and we find real
		 * work to switch to, we leave idle "off" the queue so the
		 * empty-queue path can return to it whenever needed. */
		dispq_push_locked(c, prev);
		prev->state = KT_READY;
	}
	next->state = KT_RUNNING;
	c->cpu_thread = next;
	/* Maintain the global idle mask used by IPI wake.  Setting/
	 * clearing under cpu_disp_lock means a remote waker either sees
	 * the bit set BEFORE this CPU committed to idle (so the IPI is
	 * a no-op because the CPU was about to drain its own queue
	 * anyway) or AFTER (so the IPI wakes the WFI in the idle loop). */
	uint32_t mybit = 1u << c->cpu_id;
	if (next == c->cpu_idle)
		cpu_idle_mask |=  mybit;
	else if (prev == c->cpu_idle)
		cpu_idle_mask &= ~mybit;
	/* Release the queue lock; keep IRQs masked across context_switch. */
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
			/* Free the lazily-allocated per-signal disposition
			 * table if this thread ever called sigaction. */
			if (t->sig_actions) kfree(t->sig_actions);
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

/*
 * Per-secondary-CPU scheduler bring-up.  Called from secondary_main
 * (kernel/main.c) once the core is in EL1 with MMU on and VBAR_EL1
 * installed.
 *
 * The current execution context (running on the stack allocated by
 * smp_wake_secondary) becomes this CPU's idle thread.  The idle thread
 * is never put on the dispatch queue; it's the fallback that runs when
 * there is nothing else and lets the core WFI.
 *
 * stack_base is left NULL: the stack page belongs to the idle thread
 * for the lifetime of the kernel and is never reaped.
 */
void sched_secondary_init(unsigned cpu_id)
{
	if (cpu_id >= KSCHED_NCPU) return;
	struct cpu *c = &cpus[cpu_id];

	struct kthread *idle = kmalloc(sizeof(*idle));
	if (!idle) {
		kprintf("sched: cpu %u: idle alloc failed -- parking\n",
			cpu_id);
		for (;;) __asm__ volatile ("wfi");
	}
	kmemset(idle, 0, sizeof(*idle));

	{
		unsigned long f = spin_lock_irq_save(&tid_lock);
		idle->tid = next_tid++;
		if (idle->tid < KSCHED_MAX_TID)
			tid_table[idle->tid] = idle;
		spin_unlock_irq_restore(&tid_lock, f);
	}
	idle->name       = "idle";
	idle->state      = KT_RUNNING;
	idle->stack_base = NULL;	/* never reaped; stack lives forever */

	c->cpu_id     = cpu_id;
	c->cpu_thread = idle;
	c->cpu_idle   = idle;
	/* idle is KT_RUNNING on this CPU: t_lockp = cpu_thread_lock
	 * per the discipline on struct kthread::t_lockp. */
	idle->t_lockp = &c->cpu_thread_lock;

	/* Mark this CPU as starting in its idle thread so a waker on
	 * another core knows to send a wake-up IPI when it parks work. */
	cpu_idle_mask |= (1u << cpu_id);

	/* Publish: after this, curcpu() / curthread are valid on this core. */
	set_curcpu(c);

	kprintf("sched: cpu %u up  idle tid=%u\n", cpu_id, idle->tid);
}

void kthread_exit(void)
{
	struct kthread *me = curthread;
	/* Release every file the dying thread still holds.  pipe ends
	 * inherited from the parent get their refcount dropped here so
	 * the pipe goes away naturally when both ends are closed. */
	vfs_drain_fds(me);
	kprintf("kthread: tid=%u (%s) exited\n", me->tid, me->name);

	/* Set state FIRST, then wake.  A waiter racing kthread_find
	 * after the wake must see state=KT_DEAD or it'll sleep again
	 * and nothing will wake it a second time. */
	unsigned long flags = spin_lock_irq_save(&to_reap_lock);
	me->state = KT_DEAD;
	me->next  = to_reap;
	to_reap   = me;
	spin_unlock_irq_restore(&to_reap_lock, flags);

	/* Now wake sys_wait()'ers.  kthread_find returns NULL on KT_DEAD,
	 * so the waiter will observe "tid is gone" and return 0. */
	kthread_wake_all(&thread_exit_wq);

	switch_to_next(0);
	/* unreachable: switch_to_next with requeue=0 never returns
	 * once cur is no longer in the ready queue and never woken. */
	(void)flags;
	for (;;)
		__asm__ volatile ("wfe");
}

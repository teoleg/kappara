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
#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/process.h"
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

/* ---- Scheduling classes (phase 4) ---------------------------------------
 *
 * Two classes, matching the SVR4 `sclass_ops` shape but trimmed to
 * the hooks our scheduler currently calls:
 *
 *   sys_class -- fixed priority, no quantum, never preempted by
 *                cl_tick.  Used by every kthread that isn't a user
 *                EL0 thread (uart_rx, klogd-style readers, /proc
 *                qopens, idle).
 *
 *   ts_class  -- inlined ts_dptbl-style aging.  cl_fork stamps the
 *                default priority and a fresh quantum.  cl_tick
 *                ticks the quantum down; on hit zero, demote one
 *                ts_dptbl row (priority dec by 5, floor 0) and
 *                reload the quantum.  cl_wakeup snaps priority back
 *                to `ts_slpret` (= KSCHED_PRI_TS_DEFAULT) so an
 *                I/O-bound thread always returns at default
 *                priority no matter how far it had drifted.
 *
 * The TS table is collapsed into the constants below.  The full
 * Solaris ts_dptbl has 60 rows of (quantum, tqexp, slpret, maxwait,
 * lwait); we don't yet do maxwait/lwait/load-balancing-by-age, so
 * the row is just (TS_DEMOTE_STEP, slpret).
 */
#define TS_QUANTUM_TICKS	5	/* ~50ms at HZ=100 */
#define TS_DEMOTE_STEP		5	/* tqexp -- drop 5 priorities per
					 *          quantum exhaustion */

static void sys_cl_wakeup(struct kthread *t)
{
	/* Plain SYS thread: a wakeup doesn't change priority.  The
	 * thread already has KSCHED_PRI_SYS_DEFAULT; leave it. */
	(void)t;
}

static int sys_cl_tick(struct kthread *t)
{
	/* SYS class never voluntarily yields from sched_tick.  Phase
	 * 4 keeps round-robin among SYS peers at the same priority by
	 * still letting the timer-driven kthread_yield run -- this
	 * hook only signals "preempt right now" for class reasons,
	 * which SYS never wants. */
	(void)t;
	return 0;
}

static void sys_cl_fork(struct kthread *t)
{
	t->t_pri          = KSCHED_PRI_SYS_DEFAULT;
	t->t_quantum_left = 0;
}

static void ts_cl_wakeup(struct kthread *t)
{
	/* ts_slpret: a thread blocking on I/O gets its priority
	 * snapped back to KSCHED_PRI_TS_DEFAULT regardless of how
	 * far cl_tick had demoted it.  The quantum resets too. */
	t->t_pri          = KSCHED_PRI_TS_DEFAULT;
	t->t_quantum_left = TS_QUANTUM_TICKS;
}

static int ts_cl_tick(struct kthread *t)
{
	if (t->t_quantum_left > 0)
		t->t_quantum_left--;
	if (t->t_quantum_left == 0) {
		/* ts_tqexp: demote one row, floor at KSCHED_PRI_TS_MIN. */
		int newp = t->t_pri - TS_DEMOTE_STEP;
		if (newp < KSCHED_PRI_TS_MIN) newp = KSCHED_PRI_TS_MIN;
		t->t_pri          = newp;
		t->t_quantum_left = TS_QUANTUM_TICKS;
		return 1;	/* preempt: someone may now have higher pri */
	}
	return 0;
}

static void ts_cl_fork(struct kthread *t)
{
	t->t_pri          = KSCHED_PRI_TS_DEFAULT;
	t->t_quantum_left = TS_QUANTUM_TICKS;
}

static const struct sclass_ops sys_class = {
	.name      = "SYS",
	.cl_wakeup = sys_cl_wakeup,
	.cl_tick   = sys_cl_tick,
	.cl_fork   = sys_cl_fork,
};

static const struct sclass_ops ts_class = {
	.name      = "TS",
	.cl_wakeup = ts_cl_wakeup,
	.cl_tick   = ts_cl_tick,
	.cl_fork   = ts_cl_fork,
};

const struct sclass_ops *sclass[2] = {
	[SCLASS_SYS] = &sys_class,
	[SCLASS_TS]  = &ts_class,
};

void kthread_setclass(struct kthread *t, enum sclass_id cid)
{
	if (cid != SCLASS_SYS && cid != SCLASS_TS) return;
	t->t_cid = cid;
	sclass[cid]->cl_fork(t);
}

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
	out->dispq_len = c->cpu_nrunnable;
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

/*
 * Multi-priority dispatch helpers -- SVR4 disp_t shape.
 *
 * cpu_dispq_head[p]/cpu_dispq_tail[p] is the FIFO at priority p.
 * cpu_qactmap has bit p set iff that FIFO is non-empty.
 * cpu_maxrunpri caches the highest set bit (or KSCHED_PRI_IDLE).
 * cpu_nrunnable totals every priority -- read unlocked by the load
 * balancer and /proc/cpuload.
 *
 * All four fields are touched by every push/pop and protected by
 * c->cpu_disp_lock.
 */
static inline int dispq_clamp_pri(int pri)
{
	if (pri < 0)              return 0;
	if (pri >= KSCHED_NPRI)   return KSCHED_NPRI - 1;
	return pri;
}

static inline void dispq_recompute_max(struct cpu *c)
{
	c->cpu_maxrunpri = c->cpu_qactmap
		? (int)(63 - (unsigned)__builtin_clzll(c->cpu_qactmap))
		: KSCHED_PRI_IDLE;
}

/* Append t at its priority level.  Caller MUST hold c->cpu_disp_lock. */
static void dispq_push_locked(struct cpu *c, struct kthread *t)
{
	int pri = dispq_clamp_pri(t->t_pri);
	t->next = NULL;
	if (c->cpu_dispq_tail[pri])
		c->cpu_dispq_tail[pri]->next = t;
	else
		c->cpu_dispq_head[pri] = t;
	c->cpu_dispq_tail[pri] = t;
	c->cpu_qactmap |= (uint64_t)1 << pri;
	if (pri > c->cpu_maxrunpri)
		c->cpu_maxrunpri = pri;
	c->cpu_nrunnable++;
}

/* Pop highest-priority head.  Caller MUST hold c->cpu_disp_lock. */
static struct kthread *dispq_pop_locked(struct cpu *c)
{
	if (!c->cpu_qactmap)
		return NULL;
	int pri = (int)(63 - (unsigned)__builtin_clzll(c->cpu_qactmap));
	struct kthread *t = c->cpu_dispq_head[pri];
	c->cpu_dispq_head[pri] = t->next;
	if (!c->cpu_dispq_head[pri]) {
		c->cpu_dispq_tail[pri] = NULL;
		c->cpu_qactmap &= ~((uint64_t)1 << pri);
		dispq_recompute_max(c);
	}
	t->next = NULL;
	c->cpu_nrunnable--;
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
 * balanced.  All reads of cpu_nrunnable are unlocked -- a stale
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
	if (caller->cpu_nrunnable < 2)
		return caller;

	struct cpu *best   = caller;
	unsigned    best_n = caller->cpu_nrunnable;
	for (unsigned i = 0; i < KSCHED_NCPU; i++) {
		struct cpu *v = &cpus[i];
		if (v == caller) continue;
		unsigned n = v->cpu_nrunnable;
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
	main_thread.t_pri = KSCHED_PRI_IDLE;	/* doubles as cpu 0's idle */
	main_thread.t_cid = SCLASS_SYS;
	/* Initial main_thread is the boot session leader: tid 0 / pgrp 0
	 * / session 0.  user-init (and any subsequent shell) gets the
	 * inherited 0; calling setsid() in the shell promotes it to its
	 * own session + pgrp.  Pgrp 0 signals are a no-op so transient
	 * "before anyone called setsid" state is harmless. */
	main_thread.t_pgrp    = 0;
	main_thread.t_session = 0;
	/* Phase R1: main thread is the first thread of init_process,
	 * which wraps the boot-time L0 page table.  All subsequent
	 * kthread_create calls inherit t_proc until R1c2 sys_execve
	 * starts handing out fresh processes. */
	main_thread.t_proc    = &init_process;
	main_thread.next  = NULL;

	/* Initialise every CPU's disp_t to "no runnable threads".  BSS
	 * zero already gives us qactmap=0 and nrunnable=0; we just need
	 * maxrunpri = KSCHED_PRI_IDLE (which is -1, NOT zero). */
	for (unsigned i = 0; i < KSCHED_NCPU; i++)
		cpus[i].cpu_maxrunpri = KSCHED_PRI_IDLE;

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

static struct kthread *kthread_create_internal(const char *name,
                                               void (*fn)(void *), void *arg)
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
	/* Default class is SYS; sys_execve flips child threads to TS
	 * (via kthread_setclass) right after this returns.  cl_fork
	 * sets t_pri + t_quantum_left for the class. */
	t->t_cid      = SCLASS_SYS;
	sclass[SCLASS_SYS]->cl_fork(t);
	/* Inherit session + pgrp + process from the creator -- classic
	 * Unix fork() semantics applied to our spawn-shaped model.
	 * setsid / setpgid still get to override at the receiver's
	 * end.  Phase R1: t_proc inheritance means a newly created
	 * thread shares the parent's vm_map, which is the natural
	 * pthread_create shape -- if a future caller wants a fresh
	 * address space it'll be sys_execve's job to install one. */
	{
		struct kthread *parent = curthread;
		if (parent) {
			t->t_session = parent->t_session;
			t->t_pgrp    = parent->t_pgrp;
			t->t_proc    = parent->t_proc;
		} else {
			t->t_proc    = &init_process;
		}
		if (t->t_proc) t->t_proc->refs++;
	}
	t->next       = NULL;
	/* Initial polymorphic state lock: the per-thread default.
	 * dispq_push() will transition t_lockp to the target CPU's
	 * cpu_disp_lock in a later phase; for now this is just a
	 * stable initial value so nothing reads NULL. */
	t->t_lockp    = &t->t_lock;
	t->sp         = arch_thread_init_frame((char *)stack + PAGE_SIZE,
					       fn, arg);

	/* Defer dispatch -- the matching kthread_dispatch lives below
	 * and is what plain kthread_create calls.  Callers that need
	 * to twiddle thread state after creation but before the
	 * scheduler can pick it up (R1c2: sys_execve overrides t_proc
	 * to a fresh process before letting the thread start) can call
	 * kthread_create_no_dispatch + kthread_dispatch directly. */
	return t;
}

void kthread_dispatch(struct kthread *t)
{
	if (!t) return;
	dispq_push(curcpu(), t);
	kprintf("sched: created tid=%u name=%s stack=%p\n",
		t->tid, t->name, t->stack_base);
}

struct kthread *kthread_create_no_dispatch(const char *name,
                                           void (*fn)(void *), void *arg)
{
	return kthread_create_internal(name, fn, arg);
}

struct kthread *kthread_create(const char *name, void (*fn)(void *), void *arg)
{
	struct kthread *t = kthread_create_internal(name, fn, arg);
	if (!t) return NULL;
	kthread_dispatch(t);
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
 * Quick lockless peek at cpu_qactmap before acquiring the victim's
 * lock; a stale 0 just costs one missed steal, which is harmless.
 * Highest cpu_maxrunpri across victims wins -- steal the most
 * important pending work first.
 */
static struct kthread *try_steal(struct cpu *my_cpu)
{
	struct cpu *best = NULL;
	int         best_pri = KSCHED_PRI_IDLE;
	for (unsigned i = 0; i < KSCHED_NCPU; i++) {
		struct cpu *v = &cpus[i];
		if (v == my_cpu)        continue;
		if (!v->cpu_qactmap)    continue;  /* racy but harmless */
		int p = v->cpu_maxrunpri;          /* racy but harmless */
		if (p > best_pri) {
			best     = v;
			best_pri = p;
		}
	}
	if (!best)
		return NULL;
	unsigned long f = spin_lock_irq_save(&best->cpu_disp_lock);
	struct kthread *t = dispq_pop_locked(best);
	spin_unlock_irq_restore(&best->cpu_disp_lock, f);
	return t;
}

/* Common scheduler step.  If requeue_current is set, the outgoing
 * thread goes back on THIS CPU's dispatch queue (cooperative yield);
 * if not, it has already been parked somewhere else (a wait queue,
 * exit limbo, ...) and we MUST NOT put it back.
 *
 * extra_release (Phase 5): an optional second spinlock that the caller
 * is currently holding and wants released ONLY after context_switch's
 * save phase has committed prev->sp.  Used by kthread_sleep_on to
 * carry wq->sq_lock across the switch -- a waker that grabs sq_lock
 * before prev->sp is committed could see a thread on the queue whose
 * sp is stale, exactly the steal-mid-save race that Phase 2 closed
 * for the yield path.  The OUTGOING thread stashes the pointer in
 * c->cpu_pending_release_lock; the INCOMING thread (or
 * sched_finish_switch on first-run) releases it after the requeue
 * drain, before releasing cpu_thread_lock.  NULL means "no extra lock".
 *
 * Lock discipline: spin_lock_irq_save at entry masks IRQs for the
 * entire switch; spin_unlock releases the queue lock but KEEPS IRQs
 * masked until the end.  When the local queue is empty we release the
 * queue lock (keeping IRQs masked) and try to steal from another CPU.
 * If that also turns up nothing we fall through to the per-CPU idle
 * thread so the CPU can WFI rather than spin.
 */
static void switch_to_next(int requeue_current, spinlock_t *extra_release)
{
	struct cpu *c = curcpu();
	/*
	 * Phase 2 swtch() -- Solaris discipline.
	 *
	 * (1) Acquire cpu_thread_lock.  This is the OUTGOING thread's
	 *     t_lockp by the invariant set up in phase 1.  Held across
	 *     context_switch; released by the resumed thread (or
	 *     thread_trampoline for first-run threads).
	 * (2) Acquire cpu_disp_lock for the dispatch-queue scan.
	 * (3) Pop next.  If absent, try_steal from a peer.  Re-acquire
	 *     local cpu_disp_lock if we have to.
	 * (4) If requeuing the outgoing thread, transition its t_lockp
	 *     cpu_thread_lock -> cpu_disp_lock atomically under both
	 *     locks held, then push onto the local dispq.  Other CPUs
	 *     that see the outgoing thread on the dispq are seeing a
	 *     thread whose sp will be committed AFTER context_switch
	 *     completes.  However any try_steal that pops it will read
	 *     prev->sp -- if the read happens before context_switch's
	 *     save phase commits, the stealer sees STALE sp.  That's
	 *     the steal-mid-save race.
	 *
	 *     The fix that actually closes the race: DEFER the dispq
	 *     push to AFTER context_switch's save phase, on the
	 *     resumed thread's side.  Until then, prev is stashed in
	 *     cpu_pending_requeue and its t_lockp stays at
	 *     cpu_thread_lock (which we still hold across the switch),
	 *     so any concurrent thread_lock(prev) blocks on us.
	 * (5) Transition next's t_lockp cpu_disp_lock ->
	 *     cpu_thread_lock atomically under both locks held.
	 * (6) Release cpu_disp_lock.  cpu_thread_lock stays held.
	 * (7) context_switch.  The save phase commits prev->sp; the
	 *     load phase brings in next.  On return we are next.
	 * (8) Drain c->cpu_pending_requeue: push prev onto the dispq
	 *     under cpu_disp_lock, transitioning its t_lockp from
	 *     cpu_thread_lock (which we still hold) to cpu_disp_lock.
	 * (9) Release cpu_thread_lock.  curthread is next; its
	 *     t_lockp == cpu_thread_lock per the invariant.
	 * (10) Drain to_reap as before.
	 *
	 * For first-run threads the path past (7) goes through
	 * thread_trampoline, which calls sched_finish_switch() to do
	 * steps (8)-(9) before unmasking IRQs.
	 */
	unsigned long flags = spin_lock_irq_save(&c->cpu_thread_lock);
	spin_lock(&c->cpu_disp_lock);

	struct kthread *next = dispq_pop_locked(c);
	if (!next) {
		/* Drop disp_lock so try_steal can hit other CPUs' locks
		 * without nesting.  cpu_thread_lock stays held. */
		spin_unlock(&c->cpu_disp_lock);
		next = try_steal(c);
		if (!next) {
			if (requeue_current) {
				/* Yield with nothing to do: keep running.
				 * Defensive: release any extra_release the
				 * caller passed -- in practice yield never
				 * passes one, but if a future caller does
				 * it would deadlock without this. */
				if (extra_release)
					spin_unlock(extra_release);
				spin_unlock(&c->cpu_thread_lock);
				__asm__ volatile ("msr daif, %0"
						  :: "r"(flags) : "memory");
				return;
			}
			next = c->cpu_idle;
		}
		if (!next) {
			kprintf("sched: cpu %u nothing to run; deadlock\n",
				c->cpu_id);
			for (;;)
				__asm__ volatile ("wfe");
		}
		spin_lock(&c->cpu_disp_lock);
	}

	struct kthread *prev = c->cpu_thread;
	if (next == prev) {
		spin_unlock(&c->cpu_disp_lock);
		if (extra_release)
			spin_unlock(extra_release);
		spin_unlock(&c->cpu_thread_lock);
		__asm__ volatile ("msr daif, %0" :: "r"(flags) : "memory");
		return;
	}

	/* (4) Deferred requeue of prev: stash, set KT_READY.
	 *
	 * prev->t_lockp stays at cpu_thread_lock (which we still
	 * hold).  The resumed thread will transition it to
	 * cpu_disp_lock under both locks held, then push, in step (8).
	 *
	 * For !requeue_current (sleep / exit), prev is already linked
	 * elsewhere by the caller (wait queue, to_reap) -- nothing for
	 * swtch to do with prev. */
	if (requeue_current && prev != c->cpu_idle) {
		c->cpu_pending_requeue = prev;
		prev->state = KT_READY;
	} else {
		c->cpu_pending_requeue = NULL;
	}

	/* (4b) Stash extra_release.  For sleep this is &wq->sq_lock,
	 * which the caller is currently holding.  The resumer releases
	 * it AFTER context_switch's save phase commits prev->sp, which
	 * is what closes the wake-side variant of the steal-mid-save
	 * race -- wakers spinning on sq_lock can't proceed until then. */
	c->cpu_pending_release_lock = extra_release;

	/* (5) next's t_lockp transition cpu_disp_lock -> cpu_thread_lock.
	 * We hold both, so concurrent thread_lock(next) sees the flip
	 * atomically.  After this, next is owned by cpu_thread_lock. */
	next->t_lockp = &c->cpu_thread_lock;
	next->state   = KT_RUNNING;
	c->cpu_thread = next;

	uint32_t mybit = 1u << c->cpu_id;
	if (next == c->cpu_idle)
		cpu_idle_mask |=  mybit;
	else if (prev == c->cpu_idle)
		cpu_idle_mask &= ~mybit;

	/* (6) Release disp_lock; cpu_thread_lock stays held across (7). */
	spin_unlock(&c->cpu_disp_lock);

	/* (6b) Phase R1c2: swap TTBR0_EL1 when crossing a process
	 * boundary.  Done BEFORE context_switch so the new process's
	 * page tables are live by the time we load its sp -- which
	 * sits on its kernel stack (mapped identically across all
	 * processes for now) but its ret target may reach into user
	 * pages.  TLBI is per-CPU; no broadcast needed.  Skipped when
	 * both threads share a process (the common pthread-style
	 * case) so we don't pay a TLB flush per context switch. */
	if (next->t_proc && prev->t_proc
	    && next->t_proc->vm != prev->t_proc->vm)
		mmu_vmap_switch(next->t_proc->vm);

	/* (7) The actual swap.  Save commits prev->sp before load. */
	context_switch(&prev->sp, next->sp);

	/* (8) Drain pending_requeue: push prev with t_lockp transition.
	 *
	 * We arrive here as `next`, with cpu_thread_lock held (carried
	 * across the switch).  If our predecessor stashed a thread for
	 * us to requeue, do it now.
	 *
	 * Re-read curcpu() in case we somehow migrated -- on this
	 * architecture context_switch never changes CPU, but the
	 * re-read is free insurance against future changes. */
	{
		struct cpu *c2 = curcpu();
		struct kthread *to_push = c2->cpu_pending_requeue;
		c2->cpu_pending_requeue = NULL;
		if (to_push) {
			spin_lock(&c2->cpu_disp_lock);
			to_push->t_lockp = &c2->cpu_disp_lock;
			dispq_push_locked(c2, to_push);
			spin_unlock(&c2->cpu_disp_lock);
			if (cpu_idle_mask)
				ipi_wake_idle();
		}

		/* (8b) Release the optional extra_release lock that the
		 * sleeper stashed.  Order matters: release this BEFORE
		 * cpu_thread_lock so a waker spinning on sq_lock can
		 * make progress while we still own "the running thread on
		 * this CPU is stable" via cpu_thread_lock. */
		spinlock_t *extra = c2->cpu_pending_release_lock;
		c2->cpu_pending_release_lock = NULL;
		if (extra)
			spin_unlock(extra);

		/* (9) Release cpu_thread_lock.  curthread is `next`;
		 * its t_lockp == cpu_thread_lock per the invariant, and
		 * we are that thread, so releasing the lock is what hands
		 * off "the running thread on this CPU is stable" to
		 * code outside swtch. */
		spin_unlock(&c2->cpu_thread_lock);
	}

	/* (10) Drain the to-reap list. */
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
			if (t->sig_actions) kfree(t->sig_actions);
			kfree(t);
		}
	}

	/* Restore the IRQ state from when we entered switch_to_next. */
	__asm__ volatile ("msr daif, %0" :: "r"(flags) : "memory");
}

void kthread_yield(void)
{
	switch_to_next(1, NULL);
}

/*
 * sched_finish_switch -- called by thread_trampoline (switch.S) as
 * the very first act of a brand-new thread, before it executes any
 * of its fn(arg) body.
 *
 * It does the same handoff work that swtch's resumed-thread side
 * does (steps (8) and (9) of the comment in switch_to_next).  A
 * first-run thread doesn't go through that code because its saved
 * x30 points at thread_trampoline rather than back into swtch.
 *
 * IRQ discipline: arrives with DAIF.I set (initial saved DAIF in
 * arch_thread_init_frame is 0x80).  Stays masked through the
 * dispq_push and the cpu_thread_lock release; the trampoline issues
 * `daifclr, #2` AFTER this function returns.  Without I masked, a
 * timer IRQ between trampoline entry and the unlock would re-enter
 * swtch on this CPU and deadlock on the cpu_thread_lock we still
 * hold.
 */
void sched_finish_switch(void)
{
	struct cpu *c = curcpu();
	struct kthread *to_push = c->cpu_pending_requeue;
	c->cpu_pending_requeue = NULL;
	if (to_push) {
		spin_lock(&c->cpu_disp_lock);
		to_push->t_lockp = &c->cpu_disp_lock;
		dispq_push_locked(c, to_push);
		spin_unlock(&c->cpu_disp_lock);
		if (cpu_idle_mask)
			ipi_wake_idle();
	}
	spinlock_t *extra = c->cpu_pending_release_lock;
	c->cpu_pending_release_lock = NULL;
	if (extra)
		spin_unlock(extra);
	spin_unlock(&c->cpu_thread_lock);
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
	unsigned long flags = spin_lock_irq_save(&wq->sq_lock);
	kthread_sleep_on_locked(wq, flags);
}

void kthread_sleep_on_locked(struct wait_queue *wq, unsigned long flags)
{
	/*
	 * Caller holds wq->sq_lock with IRQs masked.  We carry sq_lock
	 * across context_switch via extra_release; the INCOMING thread
	 * releases it AFTER context_switch commits our sp.  Until that
	 * point any concurrent waker spins on sq_lock and cannot observe
	 * us in a half-saved state.  See Phase 5 design notes on struct
	 * wait_queue::sq_lock.
	 */
	struct kthread *t = curthread;
	t->state      = KT_BLOCKED;
	t->waiting_on = wq;
	t->next       = wq->head;
	wq->head      = t;
	switch_to_next(0, &wq->sq_lock);
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
	unsigned long flags = spin_lock_irq_save(&wq->sq_lock);
	struct kthread *t = wq->head;
	wq->head = NULL;
	/* Mark them READY under sq_lock so a racing thread_lock(t) that
	 * sees t->t_lockp transitioned to dispq_lock by dispq_push sees
	 * KT_READY rather than stale KT_BLOCKED.  Also call cl_wakeup
	 * to let the class promote priority back to its slpret entry
	 * (TS: snap to KSCHED_PRI_TS_DEFAULT) BEFORE dispq_push reads
	 * t->t_pri to pick the queue. */
	for (struct kthread *p = t; p; p = p->next) {
		p->waiting_on = NULL;
		p->state      = KT_READY;
		sclass[p->t_cid]->cl_wakeup(p);
	}
	spin_unlock_irq_restore(&wq->sq_lock, flags);
	/* Wake each onto the WAKER's CPU.  dispq_push picks an idle peer
	 * via push-side load balancing and IPIs idle cores. */
	struct cpu *c = curcpu();
	while (t) {
		struct kthread *n = t->next;
		t->next = NULL;
		dispq_push(c, t);
		t = n;
	}
}

void kthread_wake_one(struct wait_queue *wq)
{
	unsigned long flags = spin_lock_irq_save(&wq->sq_lock);
	struct kthread *t = wq->head;
	if (t) {
		wq->head      = t->next;
		t->next       = NULL;
		t->waiting_on = NULL;
		t->state      = KT_READY;
		sclass[t->t_cid]->cl_wakeup(t);
	}
	spin_unlock_irq_restore(&wq->sq_lock, flags);
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

	/*
	 * Set sig_pending under the target's PER-THREAD sigwait_wq.sq_lock.
	 * This interlocks with sys_sigsuspend_impl, which holds the same
	 * sq_lock around its (pending & ~mask) check.  Phase 6: closes
	 * the sigsuspend lost-wakeup window where a signal arriving
	 * between check and sleep would otherwise be ignored.
	 *
	 * For non-sigsuspend targets (the common case), this is a
	 * single uncontended acquire -- sigwait_wq is per-thread, so
	 * different signalers targeting different threads don't
	 * collide, and a thread not in sigsuspend has nobody else
	 * touching its sigwait_wq.sq_lock.
	 */
	spin_lock(&t->sigwait_wq.sq_lock);
	t->sig_pending |= SIGBIT(sig);

	/* If t is asleep on its OWN sigwait_wq (i.e. it's in
	 * sys_sigsuspend), surgically remove it here while we still
	 * hold sigwait_wq.sq_lock -- avoids dropping and re-acquiring.
	 * The signaler that wakes a sigsuspended thread does the
	 * normal wake + push afterwards. */
	int woke_from_sigwait = 0;
	if (t->state == KT_BLOCKED && t->waiting_on == &t->sigwait_wq) {
		struct wait_queue *wq = &t->sigwait_wq;
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
		sclass[t->t_cid]->cl_wakeup(t);
		woke_from_sigwait = 1;
		needs_dispq_push  = 1;
	}
	spin_unlock(&t->sigwait_wq.sq_lock);

	/*
	 * If t was BLOCKED on some OTHER wait queue (stream_read,
	 * sys_wait, ...), surgically unlink it from THAT queue using
	 * the existing path.  We re-read waiting_on because we don't
	 * hold that wq's sq_lock yet (lock order: sigwait_wq.sq_lock
	 * first, then any other wq's sq_lock is acquired later).
	 */
	if (!woke_from_sigwait) {
		struct wait_queue *wq = NULL;
		if (t->state == KT_BLOCKED && t->waiting_on)
			wq = t->waiting_on;
		if (wq) {
			spin_lock(&wq->sq_lock);
			if (t->state == KT_BLOCKED && t->waiting_on == wq) {
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
				sclass[t->t_cid]->cl_wakeup(t);
				needs_dispq_push = 1;
			}
			spin_unlock(&wq->sq_lock);
		}
	}

	irq_restore(flags);
	if (needs_dispq_push)
		dispq_push(curcpu(), t);
	return 0;
}

int kthread_signal_pgrp(unsigned pgrp, unsigned sig)
{
	if (pgrp == 0) return 0;	/* "no group" -- nothing to signal */
	int n = 0;
	for (unsigned i = 0; i < KSCHED_MAX_TID; i++) {
		struct kthread *t = kthread_at(i);
		if (!t) continue;
		if (t->state == KT_DEAD) continue;
		if (t->t_pgrp != pgrp) continue;
		if (kthread_signal(t, sig) == 0) n++;
	}
	return n;
}

void sched_tick(void)
{
	/* Each tick: drain any deferred STREAMS work, then consult the
	 * scheduling class.  Phase 8: honour the cl_tick preempt
	 * signal.  cl_tick returns 1 for TS threads whose quantum just
	 * expired (priority was demoted); we also yield if the dispq
	 * has a peer at >= curthread's priority so same-priority
	 * round-robin still works (a TS thread sharing pri with a peer,
	 * or a SYS thread whose pri-60 peer just became runnable, gets
	 * rotated even though cl_tick returns 0).
	 *
	 * A CPU-bound SYS thread with no peers at its priority
	 * therefore runs uninterrupted until it voluntarily blocks --
	 * which is what SYS class is for. */
	streams_run();
	struct kthread *cur = curthread;
	if (!cur)
		return;
	int preempt = sclass[cur->t_cid]->cl_tick(cur);
	if (preempt || curcpu()->cpu_maxrunpri >= cur->t_pri)
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
	idle->t_pri      = KSCHED_PRI_IDLE;	/* never enqueued; marker */
	idle->t_cid      = SCLASS_SYS;
	idle->t_proc     = &init_process;
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

/* ---- exit-status table ------------------------------------------------- */
/* Small static table so sys_wait can retrieve a thread's exit status
 * even after the kthread has been reaped (stack freed).  Sized for the
 * typical max number of threads that can exit between a spawn and a
 * wait; collisions only occur if more than EXIT_TAB_N distinct tids
 * exit before being waited on -- not a real concern on this kernel. */
#define EXIT_TAB_N	64
static struct { unsigned tid; int status; } exit_tab[EXIT_TAB_N];
static spinlock_t exit_tab_lock = SPINLOCK_INIT;

static void exit_tab_store(unsigned tid, int status)
{
	unsigned long fl = spin_lock_irq_save(&exit_tab_lock);
	unsigned idx = tid % EXIT_TAB_N;
	exit_tab[idx].tid    = tid;
	exit_tab[idx].status = status;
	spin_unlock_irq_restore(&exit_tab_lock, fl);
}

int kthread_exit_status(unsigned tid)
{
	unsigned long fl = spin_lock_irq_save(&exit_tab_lock);
	unsigned idx = tid % EXIT_TAB_N;
	int st = (exit_tab[idx].tid == tid) ? exit_tab[idx].status : 0;
	spin_unlock_irq_restore(&exit_tab_lock, fl);
	return st;
}

void kthread_exit(int status)
{
	struct kthread *me = curthread;
	/* Store exit status before going KT_DEAD so sys_wait can read
	 * it even after the kthread is reaped. */
	me->t_exit_status = status;
	exit_tab_store(me->tid, status);
	/* Release every file the dying thread still holds.  pipe ends
	 * inherited from the parent get their refcount dropped here so
	 * the pipe goes away naturally when both ends are closed. */
	vfs_drain_fds(me);
	kprintf("kthread: tid=%u (%s) exited\n", me->tid, me->name);

	/*
	 * Lock acquisition order:
	 *   to_reap_lock          -- carried across the switch so a
	 *                            remote drain can't pmm_free our
	 *                            stack mid-save (phase 5b).
	 *   thread_exit_wq.sq_lock -- set state=KT_DEAD AND wake any
	 *                            sys_wait() waiters under the same
	 *                            lock the waiter takes for its
	 *                            check-then-sleep window.  Without
	 *                            this, a waiter could read
	 *                            state=KT_RUNNING, decide to sleep,
	 *                            then sleep AFTER our wake (which
	 *                            saw an empty wq) -- a classic
	 *                            lost-wakeup hang.  sys_wait_impl
	 *                            and kthread_sleep_on_locked are
	 *                            the matched pair.
	 */
	unsigned long flags = irq_save_and_disable();
	spin_lock(&to_reap_lock);
	spin_lock(&thread_exit_wq.sq_lock);

	me->state = KT_DEAD;
	me->next  = to_reap;
	to_reap   = me;

	/* Inline wake_all so we can do it under the sq_lock we already
	 * hold (re-acquiring would deadlock on this non-recursive lock).
	 * Set state=KT_READY and unlink under sq_lock; dispq_push happens
	 * AFTER we release sq_lock, since dispq_push acquires disp_lock
	 * and we want the lock order to stay sq_lock -> disp_lock with
	 * no inversion via the resumer drain path. */
	struct kthread *t = thread_exit_wq.head;
	thread_exit_wq.head = NULL;
	for (struct kthread *p = t; p; p = p->next) {
		p->waiting_on = NULL;
		p->state      = KT_READY;
	}
	spin_unlock(&thread_exit_wq.sq_lock);

	struct cpu *c = curcpu();
	while (t) {
		struct kthread *n = t->next;
		t->next = NULL;
		dispq_push(c, t);
		t = n;
	}

	switch_to_next(0, &to_reap_lock);
	/* unreachable: switch_to_next with requeue=0 never returns
	 * once cur is no longer in the ready queue and never woken. */
	(void)flags;
	for (;;)
		__asm__ volatile ("wfe");
}

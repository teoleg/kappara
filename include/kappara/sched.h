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

struct kthread {
	void          *sp;		/* saved kernel SP for context switch */
	void          *stack_base;	/* page allocated for the kernel stack */
	const char    *name;
	unsigned       tid;
	enum kt_state  state;
	struct kthread *next;
};

extern struct kthread *cur;

void            sched_init(void);
struct kthread *kthread_create(const char *name, void (*fn)(void *), void *arg);
void            kthread_yield(void);
void            kthread_exit(void) __attribute__((noreturn));
void            sched_tick(void);

/*
 * Arch-specific hook implemented in arch/<arch>/thread.c.  Lays
 * down the synthetic saved-register frame at stack_top so that
 * the first context_switch into the new thread pops fn/arg into
 * the callee-saved registers and "returns" into thread_trampoline.
 * Returns the SP value the thread should resume at.
 */
void *arch_thread_init_frame(void *stack_top, void (*fn)(void *), void *arg);

#endif

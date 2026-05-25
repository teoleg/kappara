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

#endif

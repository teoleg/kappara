#include <stdint.h>

#include "kappara/kmem.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/sched.h"

extern void context_switch(void **save_sp, void *new_sp);
extern void thread_trampoline(void);

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

	/*
	 * Build the same 96-byte saved frame that context_switch expects:
	 *   [+0]  x19,x20  (fn, arg)
	 *   [+16] x21,x22
	 *   [+32] x23,x24
	 *   [+48] x25,x26
	 *   [+64] x27,x28
	 *   [+80] x29,lr   (fp=0, lr=thread_trampoline)
	 */
	uint64_t *sp = (uint64_t *)((char *)stack + PAGE_SIZE);
	sp -= 12;
	sp[0]  = (uint64_t)(uintptr_t)fn;
	sp[1]  = (uint64_t)(uintptr_t)arg;
	sp[2]  = 0; sp[3]  = 0;
	sp[4]  = 0; sp[5]  = 0;
	sp[6]  = 0; sp[7]  = 0;
	sp[8]  = 0; sp[9]  = 0;
	sp[10] = 0;
	sp[11] = (uint64_t)(uintptr_t)thread_trampoline;
	t->sp = sp;

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

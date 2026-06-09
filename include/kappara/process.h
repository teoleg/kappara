/*
 * include/kappara/process.h -- Unix process structure
 *
 * A process owns an address space + a set of threads + a row in the
 * process table.  kappara's threading model up to R1 fused thread and
 * process into one struct kthread because every EL0 thread shared
 * one global 2 MB user VA.  R1 is the architectural shift: each
 * process gets its own page tables, and a kthread now points at the
 * process it belongs to via t_proc.
 *
 * Phase R1 chunk 1 (this header): data model + plumbing.  All
 * kthreads start out pointing at a single `init_process` whose
 * vm_map wraps the boot-time global l0_table, so behaviour is
 * byte-identical to pre-R1 today.  Subsequent chunks:
 *
 *   R1c2 -- sys_execve allocates a fresh vm_map (own L0 + per-user
 *           L2 tables).  switch_to_next swaps TTBR0_EL1 when t_proc
 *           changes.
 *   R1c3 -- process destruction tears the vm_map down.
 *   R5   -- fork() copies the parent's vm_map (eager v1, COW v2).
 */
#ifndef KAPPARA_PROCESS_H
#define KAPPARA_PROCESS_H

#include <stdint.h>

#include "kappara/spinlock.h"

struct kthread;	/* fwd; defined in kappara/sched.h */

/*
 * Per-process virtual memory map.
 *
 * The l0_phys field is the physical address of the top-level page
 * table that becomes TTBR0_EL1 when a thread of this process runs.
 * The boot kernel's table (uts/aarch64/mmu.c's l0_table) backs the
 * init_process's vm_map directly -- no copy.  Newly exec'd processes
 * allocate their own L0 table page (4 KiB) and populate it.
 *
 * The map struct intentionally stays opaque to most kernel code;
 * only the arch MMU layer + execve + fork/exit care about its
 * internals.  Everyone else just compares vm_map pointers to decide
 * whether a context switch needs to swap address spaces.
 */
struct vm_map {
	uint64_t  l0_phys;	/* TTBR0_EL1 value for this process */
	int       refs;		/* threads sharing this map */
	spinlock_t lock;
};

struct process {
	unsigned        pid;
	struct vm_map  *vm;
	struct process *parent;
	int             exit_status;
	int             refs;		/* threads + waiters still using it */
};

/*
 * The bootstrap process all kthreads belong to until R1c2 lands a
 * real execve.  Defined in kernel/process.c; init_vm_map points at
 * the boot l0_table.
 */
extern struct process init_process;
extern struct vm_map  init_vm_map;

/* Stamp init_vm_map.l0_phys from the live MMU boot table.  Must be
 * called between mmu_init() and sched_init() -- main_thread's
 * t_proc dereferences init_process from sched_init onwards. */
void process_init(void);

/*
 * Allocate a fresh struct process bound to `vm`.  vm->refs is bumped.
 * Returns NULL on alloc failure.  Caller is responsible for assigning
 * the new process to one or more kthreads via t_proc.
 */
struct process *process_alloc(struct vm_map *vm);

/*
 * Drop a reference on `p`.  When refs hit 0 the vm_map ref is also
 * dropped (which may tear it down) and `p` is freed.  Called from
 * kthread_exit's resumer after the dying thread's stack is free.
 */
void process_put(struct process *p);

/*
 * vm_map refcounting.  fork() will bump refs (or copy the map and
 * leave refs == 1 on each side); exit drops refs.  Last release
 * tears down the per-process page tables.
 */
struct vm_map *vm_map_get(struct vm_map *vm);
void           vm_map_put(struct vm_map *vm);

#endif

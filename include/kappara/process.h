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
	uint64_t  l1_phys;	/* for teardown; arch-allocated by
				 * mmu_vmap_create on aarch64 */
	uint64_t  l2_phys;	/* L2 we mutate when mapping user pages */
	/* R6: per-process heap break.  Starts at EXEC_HEAP_VA after
	 * exec; sys_brk advances it by allocating PMM pages and
	 * installing 4 KB mappings via mmu_vmap_map_user_4k.  Init
	 * shells (boot vm_map) carry heap_brk=0 and reject brk. */
	uint64_t  heap_brk;
	/* R6: sys_spawn stack-slot counter, per-process so spawned
	 * threads in one process don't collide with another process's
	 * slots even when both share the same EL0 stack VA range. */
	unsigned  spawn_next;
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

/*
 * Allocate a fresh vm_map with empty user VA + a duplicated kernel
 * mapping.  Refs = 1 on return.  NULL on allocation failure.  The
 * arch backend (mmu_vmap_create) pulls 3 pages from pmm for L0/L1/L2;
 * the matching vm_map_put will release them via mmu_vmap_destroy.
 */
struct vm_map *vm_map_create(void);

/* Boot-time self-test: create a vm_map, verify it carries the kernel
 * mappings + TTBR0 swap-and-back works without crashing.  Runs after
 * the existing vt/ldterm/tty/ldterm-ioc battery. */
void process_selftest(void);

#endif

/*
 * kernel/process.c -- Unix process bookkeeping
 *
 * Phase R1 chunk 1.  See include/kappara/process.h for the bigger
 * picture; this file implements:
 *
 *   - init_vm_map / init_process: the bootstrap entities that every
 *     kthread points at until R1c2 lands a real execve.
 *
 *   - process_alloc / process_put -- ref-counted lifecycle.
 *
 *   - vm_map_get / vm_map_put -- ref-counted shared-map lifecycle.
 *     vm_map_put on the last reference DOESN'T free the boot
 *     init_vm_map (which is statically allocated); for any other
 *     map it kfrees the struct after dropping the L0 page back to
 *     pmm.  R1c2's execve fills in the per-process tear-down.
 *
 * No locks taken in v1: process creation is serialised through
 * sys_execve which is itself serialised by the shell's spawn-then-
 * wait loop.  When real concurrent forks land, an spinlock around
 * the pid_table + per-vm_map refs becomes necessary.
 */

#include <stdint.h>

#include "kappara/kmem.h"
#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/process.h"
#include "kappara/string.h"

struct vm_map init_vm_map = {
	.l0_phys = 0,	/* filled in at process_init -- see below */
	.refs    = 1,	/* held by init_process */
	.lock    = SPINLOCK_INIT,
};

struct process init_process = {
	.pid          = 1,
	.vm           = &init_vm_map,
	.parent       = 0,
	.exit_status  = 0,
	.refs         = 1,	/* held by main_thread + every kthread until
				 * R1c2 starts assigning per-process VMs */
};

static unsigned next_pid = 2;

void process_init(void)
{
	/* mmu_init has already run by the time we get here; the boot
	 * L0 page table's physical address is stable for the lifetime
	 * of the kernel.  Stamp it into init_vm_map so init_process's
	 * threads continue running on the same translation regime
	 * they booted on. */
#ifdef __aarch64__
	init_vm_map.l0_phys = mmu_boot_l0_phys();
#else
	init_vm_map.l0_phys = 0;
#endif
}

struct vm_map *vm_map_get(struct vm_map *vm)
{
	if (vm) vm->refs++;
	return vm;
}

void vm_map_put(struct vm_map *vm)
{
	if (!vm) return;
	if (--vm->refs > 0) return;
	if (vm == &init_vm_map) {
		/* Statically allocated, owns the boot page tables --
		 * never freed.  Reaching zero refs would be a bug
		 * (init_process holds one for the lifetime of the
		 * kernel), so flag it. */
		kprintf("vm_map_put: init_vm_map refs hit 0 -- bug\n");
		init_vm_map.refs = 1;
		return;
	}
	/* R1c2: free the L0 / L1 / L2 / L3 page-table pages this map
	 * owns.  For now, free the kmalloc'd struct only -- exec
	 * hasn't started allocating page tables yet, so a leak here
	 * is impossible in v1. */
	kfree(vm);
}

struct process *process_alloc(struct vm_map *vm)
{
	if (!vm) return 0;
	struct process *p = kmalloc(sizeof(*p));
	if (!p) return 0;
	kmemset(p, 0, sizeof(*p));
	p->pid    = next_pid++;
	p->vm     = vm_map_get(vm);
	p->parent = 0;
	p->refs   = 1;
	return p;
}

void process_put(struct process *p)
{
	if (!p) return;
	if (p == &init_process) {
		/* Statically allocated; never freed. */
		return;
	}
	if (--p->refs > 0) return;
	vm_map_put(p->vm);
	kfree(p);
}

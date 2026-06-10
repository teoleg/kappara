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
	.l0_phys  = 0,	/* filled in at process_init -- see below */
	.heap_brk = 0,	/* boot map has no heap; sys_brk rejects */
	.refs     = 1,	/* held by init_process */
	.lock     = SPINLOCK_INIT,
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
	/* Free the L0 / L1 / L2 page-table pages this map owns (R1c2
	 * allocated them via mmu_vmap_create).  Then the struct. */
#ifdef __aarch64__
	mmu_vmap_destroy(vm);
#endif
	kfree(vm);
}

struct vm_map *vm_map_create(void)
{
	struct vm_map *vm = kmalloc(sizeof(*vm));
	if (!vm) return 0;
	kmemset(vm, 0, sizeof(*vm));
	vm->refs     = 1;
	vm->heap_brk = 0;	/* sys_execve / sys_fork stamp this */
#ifdef __aarch64__
	if (mmu_vmap_create(vm) < 0) {
		kfree(vm);
		return 0;
	}
#endif
	return vm;
}

/* ---- selftest --------------------------------------------------------- */

void process_selftest(void)
{
	/* R1c2 mechanism check.  Allocate a fresh vm_map, prove its
	 * tables are populated with the kernel mappings, install a
	 * fake user mapping, swap TTBR0 to it briefly, swap back to
	 * init.  No actual EL0 access -- that comes with R2 once
	 * sys_execve loads ELFs into per-process address spaces. */
	int ok = 1;

	struct vm_map *vm = vm_map_create();
	if (!vm) {
		kprintf("process: SELFTEST FAIL vm_map_create\n");
		return;
	}
	if (!vm->l0_phys || !vm->l1_phys || !vm->l2_phys) {
		kprintf("process: SELFTEST FAIL l0=%lx l1=%lx l2=%lx\n",
		        (unsigned long)vm->l0_phys,
		        (unsigned long)vm->l1_phys,
		        (unsigned long)vm->l2_phys);
		ok = 0; goto done;
	}

	/* The fresh L2 should carry the boot kernel mappings -- the
	 * 2 MB block covering kmain (whose address we have via
	 * &process_selftest) had better be valid. */
#ifdef __aarch64__
	uint64_t self = (uint64_t)(uintptr_t)&process_selftest;
	uint64_t *l2  = (uint64_t *)(uintptr_t)vm->l2_phys;
	unsigned idx  = (unsigned)(self >> 21) & 0x1ff;
	if (!(l2[idx] & 1)) {
		kprintf("process: SELFTEST FAIL kernel L2[%u] not valid\n",
		        idx);
		ok = 0; goto done;
	}

	/* Switch TTBR0 to the new vm, then back.  If the kernel L2
	 * copy was correct we'll execute the swap-back path through
	 * the new mapping without crashing. */
	mmu_vmap_switch(vm);
	mmu_vmap_switch(&init_vm_map);
#endif

done:
	vm_map_put(vm);
	if (ok)
		kprintf("process: selftest PASS\n");
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

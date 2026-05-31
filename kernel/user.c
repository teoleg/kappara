/*
 * kernel/user.c -- AArch64 EL0 userspace, first cut
 * =================================================
 *
 * What this file is
 * -----------------
 * The minimum machinery to get a thread running at EL0 (user mode)
 * and back into the kernel via SVC.  Once it works, ksh + future
 * processes can move "out of the kernel" -- but for this first cut
 * we just want to prove the boundary itself.
 *
 * Outline
 * -------
 *
 *   user_init()                              (from kmain, after mmu_init)
 *       |
 *       v
 *   copy user_program + user_msg into user_storage (kernel-VA write)
 *   ic iallu + dsb + isb                     (instructions become visible)
 *   mmu_map_user_2mb(USER_VA, &user_storage) (publish to EL0)
 *
 *   user_spawn()                             (creates a kthread)
 *       |
 *       v  (when the scheduler picks the thread)
 *   user_thread_main()
 *       |
 *       v
 *   aarch64_enter_userspace(entry, sp_el0)
 *       msr SP_EL0, ELR_EL1, SPSR_EL1
 *       eret -> EL0 at user code
 *
 *   user code (running in EL0):
 *       mov x0, #msg_addr      ; pointer into user page
 *       mov x8, #SYS_log
 *       svc #0                  ; trap to EL1
 *   kernel:
 *       vectors.S VEC_SYNC_LO64 entry (offset 0x400)
 *       trap_dispatch, ESR.EC=0x15  (SVC from AArch64 lower EL)
 *       syscall_dispatch -> sys_log
 *       kprintf reads the user pointer (same TTBR0)
 *       eret -> back to EL0 next instruction (b .)
 *   user code: spins forever in EL0
 *
 * Backing storage and address spaces
 * ----------------------------------
 * We have one TTBR0 with kernel + user mappings co-resident; the
 * AP[2:1]=01 bit on the user 2 MB block is what makes it accessible
 * from EL0.  No TTBR1 split yet, no per-process page tables.
 *
 * user_storage is a 2 MB-aligned BSS region; mmu_map_user_2mb
 * remaps VA 0x10000000..0x10200000 to user_storage's PA with user
 * permissions, so the EL0 view at VA 0x10000000 == user_storage's
 * kernel-VA contents.
 *
 * Cache maintenance
 * -----------------
 * We just wrote bytes to user_storage as DATA.  The CPU's I-cache
 * has no entries for that PA yet, but it WILL fetch and cache once
 * EL0 starts executing.  To make sure the I-cache pulls our fresh
 * bytes (and not whatever happened to be in cache from before),
 * we do an IC IALLU + DSB ISH + ISB after the writes.  Heavy-handed
 * vs. per-VA IC IVAU but trivially correct for this scale.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/kmem.h"
#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/string.h"
#include "kappara/syscall.h"
#include "kappara/user.h"

/* User VA where we'll publish the program.  Must be 2 MB aligned. */
#define USER_VA		0x10000000UL
#define USER_SIZE	0x00200000UL
#define USER_STACK_TOP	(USER_VA + USER_SIZE)

/*
 * Backing storage: a 2 MB-aligned slab in BSS.  This is the physical
 * memory the user 2 MB VA window will eventually map to.
 */
__attribute__((aligned(0x200000)))
static unsigned char user_storage[USER_SIZE];

/*
 * The user init binary, embedded into the kernel image by
 * arch/aarch64/userblob.S via .incbin "build/user/init.bin".  The
 * sources live in user/, the build rules in Makefile.  Bytes start
 * with _start (linker.ld in user/ forces .text._start first), so
 * entry is USER_VA + 0.
 */
extern char user_blob_start[];
extern char user_blob_end[];

extern void aarch64_enter_userspace(uint64_t entry, uint64_t sp_el0,
				    uint64_t arg);

void user_init(void)
{
	size_t blob_len = (size_t)(user_blob_end - user_blob_start);
	if (blob_len > USER_SIZE) {
		kprintf("user: init binary too big (%lu > %lu)\n",
			(unsigned long)blob_len, (unsigned long)USER_SIZE);
		return;
	}
	kmemcpy(user_storage, user_blob_start, blob_len);

	/*
	 * Cache maintenance.  user_storage was last touched as DATA;
	 * the CPU's I-cache has no entries for it yet, but if it ever
	 * speculatively prefetched stale lines they'd be invalid now.
	 * DC CVAU pushes the new bytes out of D-cache to unification;
	 * IC IALLU drops any I-cache lines that might've been brought
	 * in.  DSB ISH + ISB seal the order.
	 */
	__asm__ volatile (
		"dsb	ish\n"
		"ic	iallu\n"
		"dsb	ish\n"
		"isb\n"
		::: "memory");

	/* Publish to EL0 at USER_VA. */
	mmu_map_user_2mb(USER_VA, (uintptr_t)&user_storage[0]);

	kprintf("user: init binary loaded (%lu bytes) at PA=%p, "
		"EL0 entry VA=0x%lx\n",
		(unsigned long)blob_len,
		(void *)&user_storage[0],
		(unsigned long)USER_VA);
}

static void user_thread_main(void *arg)
{
	(void)arg;
	kprintf("user: kthread entering EL0 (entry=0x%lx, sp=0x%lx)\n",
		(unsigned long)USER_VA, (unsigned long)USER_STACK_TOP);
	aarch64_enter_userspace(USER_VA, USER_STACK_TOP, 0);
	/* unreachable */
}

void user_spawn(void)
{
	kthread_create("user-init", user_thread_main, NULL);
}

/*
 * Multi-process support.  Every spawned user thread shares the same
 * user address space (single 2 MB region at USER_VA), but each gets
 * its own user-mode stack carved out from the top of that region.
 * Stack 0 (USER_STACK_TOP) belongs to init; the spawn pool sits
 * below it at 64 KB intervals.
 *
 *     USER_VA + USER_SIZE  --+
 *                            |  64 KB  init stack
 *                            +--
 *                            |  64 KB  spawn[0] stack
 *                            +--
 *                            |  64 KB  spawn[1] stack
 *                            +-- ...
 *
 * With 64 KB stacks we get (USER_SIZE / 64 KB) - 1 = 31 spawn slots.
 * Plenty for a learning OS.
 */

#define SPAWN_STACK_SIZE	0x10000UL
#define SPAWN_MAX		((USER_SIZE / SPAWN_STACK_SIZE) - 1)

static unsigned spawn_next;

struct spawn_args {
	uint64_t entry;
	uint64_t sp;
	uint64_t arg;
};

static void spawn_thread_main(void *p)
{
	struct spawn_args a = *(struct spawn_args *)p;
	kfree(p);
	kprintf("user: spawn entering EL0 (entry=0x%lx, sp=0x%lx, arg=0x%lx)\n",
		(unsigned long)a.entry, (unsigned long)a.sp,
		(unsigned long)a.arg);
	aarch64_enter_userspace(a.entry, a.sp, a.arg);
	/* unreachable */
}

long sys_spawn_impl(uint64_t entry, uint64_t arg)
{
	if (spawn_next >= SPAWN_MAX) {
		kprintf("sys_spawn: pool exhausted\n");
		return -1;
	}
	/* Reject entry points outside the user code region.  Bounds
	 * are the same 2 MB block we publish to EL0. */
	if (entry < USER_VA || entry >= USER_VA + USER_SIZE) {
		kprintf("sys_spawn: entry 0x%lx not in user range\n",
			(unsigned long)entry);
		return -1;
	}

	unsigned slot = ++spawn_next;	/* 1..SPAWN_MAX */
	struct spawn_args *a = kmalloc(sizeof(*a));
	if (!a) return -1;
	a->entry = entry;
	a->sp    = USER_VA + USER_SIZE - (uint64_t)slot * SPAWN_STACK_SIZE;
	a->arg   = arg;

	struct kthread *t = kthread_create("spawn", spawn_thread_main, a);
	if (!t) return -1;

	/* Hand the child a copy of the parent's fd table so it inherits
	 * pipes, the console, and anything else the parent had open --
	 * the fork()-ish piece of spawn that makes pipework actually
	 * compose. */
	kthread_inherit_fds(t, cur);
	return (long)t->tid;
}

void sys_exit_impl(void) __attribute__((noreturn));
void sys_exit_impl(void)
{
	kthread_exit();
	/* kthread_exit doesn't return, but the attribute helps the
	 * caller's control-flow analysis. */
	for (;;)
		;
}

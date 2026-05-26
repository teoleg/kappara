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

/* Offset within the user page where the message string lives. */
#define MSG_OFFSET	0x40

/*
 * Backing storage: a 2 MB-aligned slab in BSS.  This is the physical
 * memory the user 2 MB VA window will eventually map to.
 */
__attribute__((aligned(0x200000)))
static unsigned char user_storage[USER_SIZE];

/*
 * Hand-coded AArch64 user program.  Five instructions / 20 bytes:
 *
 *   movz x0, #0x40                ; low 16 bits of msg addr
 *   movk x0, #0x1000, lsl #16     ; high 16 -> x0 = 0x10000040
 *   movz x8, #0                   ; SYS_log
 *   svc  #0                       ; trap to kernel
 *   b    .                        ; spin forever in EL0
 *
 * Encoding values cross-checked against the ARMv8 instruction reference.
 */
static const uint32_t user_program[] = {
	0xd2800800,	/* movz x0, #0x40                  */
	0xf2a20000,	/* movk x0, #0x1000, lsl #16       */
	0xd2800008,	/* movz x8, #0   (SYS_log)         */
	0xd4000001,	/* svc  #0                          */
	0x14000000,	/* b    .                           */
};

static const char user_msg[] = "hello from EL0 userspace!";

extern void aarch64_enter_userspace(uint64_t entry, uint64_t sp_el0);

void user_init(void)
{
	/* Lay the program down at offset 0. */
	kmemcpy(user_storage, user_program, sizeof(user_program));

	/* And the message at MSG_OFFSET. */
	kmemcpy(user_storage + MSG_OFFSET, user_msg, sizeof(user_msg));

	/* Make the I-cache see the new code: clean+invalidate by VA
	 * is more surgical, but iallu is fine at this scale and avoids
	 * a per-line loop. */
	__asm__ volatile (
		"dsb	ish\n"
		"ic	iallu\n"
		"dsb	ish\n"
		"isb\n"
		::: "memory");

	/* Publish to EL0 at USER_VA. */
	mmu_map_user_2mb(USER_VA, (uintptr_t)&user_storage[0]);

	kprintf("user: program at PA=%p VA=0x%lx; msg at VA=0x%lx\n",
		(void *)&user_storage[0],
		(unsigned long)USER_VA,
		(unsigned long)(USER_VA + MSG_OFFSET));
}

static void user_thread_main(void *arg)
{
	(void)arg;
	kprintf("user: kthread entering EL0 (entry=0x%lx, sp=0x%lx)\n",
		(unsigned long)USER_VA, (unsigned long)USER_STACK_TOP);
	aarch64_enter_userspace(USER_VA, USER_STACK_TOP);
	/* unreachable */
}

void user_spawn(void)
{
	kthread_create("user-init", user_thread_main, NULL);
}

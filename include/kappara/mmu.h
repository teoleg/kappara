/*
 * include/kappara/mmu.h -- MMU bring-up entry point
 *
 * After mmu_init() the CPU is translating every address through the
 * identity-mapped page tables and has its caches on.  Until then,
 * the kernel runs uncached on raw physical addresses.  See
 * arch/aarch64/mmu.c for the page-table tree and attribute encoding.
 */
#ifndef KAPPARA_MMU_H
#define KAPPARA_MMU_H

void mmu_init(void);

#ifdef __aarch64__
/* Per-CPU MMU enable for secondaries: program SYSREGs from the
 * shared page tables core 0 already built, then turn the MMU on.
 * Same effect as the per-CPU portion of mmu_init(). */
void mmu_enable_this_cpu(void);
#endif

#ifdef __aarch64__
/* Remap one 2 MB region (va must be 2 MB aligned, must lie in the
 * low 1 GB on AArch64) as a Normal-cacheable user-RW block.
 * Used to publish a user code/data page so EL0 can read+execute it. */
void mmu_map_user_2mb(unsigned long va, unsigned long pa);

/* Phase R1: physical address of the boot-time L0 page table.  init
 * process's vm_map wraps this -- TTBR0_EL1 is loaded from it on every
 * CPU's MMU enable.  Future per-process L0s are independent
 * allocations. */
unsigned long mmu_boot_l0_phys(void);
#endif

#endif

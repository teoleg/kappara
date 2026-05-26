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
/* Remap one 2 MB region (va must be 2 MB aligned, must lie in the
 * low 1 GB on AArch64) as a Normal-cacheable user-RW block.
 * Used to publish a user code/data page so EL0 can read+execute it. */
void mmu_map_user_2mb(unsigned long va, unsigned long pa);
#endif

#endif

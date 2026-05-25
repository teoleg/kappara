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

#endif

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

/* Identity-map a 1 GB Device block via L1 (kernel-only RW, no exec).
 * The argument is rounded down to 1 GB; on return the containing 1 GB
 * window is reachable from EL1 as MMIO.  Used for high-mem MMIO
 * regions (e.g. PCIe ECAM at 0x4010000000 on QEMU virt with
 * highmem=on or on AWS Graviton) that fall outside the 0..2 GB
 * window the boot identity map already covers. */
void mmu_map_device_1gb(unsigned long va);

/* Phase R1: physical address of the boot-time L0 page table.  init
 * process's vm_map wraps this -- TTBR0_EL1 is loaded from it on every
 * CPU's MMU enable.  Future per-process L0s are independent
 * allocations. */
unsigned long mmu_boot_l0_phys(void);

/* Phase R1c2 per-process vm_map page tables.  Each call sequence is:
 *
 *   struct vm_map vm = { 0 };
 *   mmu_vmap_create(&vm);                  // allocate L0/L1/L2 + dup kernel
 *   mmu_vmap_map_user_2mb(&vm, va, pa);    // wire user code/data
 *   ... later ...
 *   mmu_vmap_switch(&vm);                  // make it live on this CPU
 *   ... swap back ...
 *   mmu_vmap_destroy(&vm);                 // free pages
 *
 * Defined in arch/aarch64/mmu.c; struct vm_map's storage for L0/L1/L2
 * physical addresses lives in include/kappara/process.h. */
struct vm_map;	/* fwd; defined in kappara/process.h */
int  mmu_vmap_create     (struct vm_map *vm);
void mmu_vmap_destroy    (struct vm_map *vm);
void mmu_vmap_map_user_2mb(struct vm_map *vm,
                           unsigned long va, unsigned long pa);
void mmu_vmap_switch     (const struct vm_map *vm);

/* R6: 4 KB user-page mappings, backed by PMM pages.  Each call into
 * a fresh 2 MB region allocates an L3 table on demand.  Returns 0 on
 * success, -1 if PMM is exhausted. */
int      mmu_vmap_map_user_4k    (struct vm_map *vm,
                                  unsigned long va, unsigned long pa);

/* Walk vm's page tables for `va` and return the kernel-side address
 * the user PA is identity-mapped at (so kernel code can read/write
 * the user page directly).  Returns NULL if the VA isn't mapped. */
void    *mmu_vmap_user_va_to_kva (const struct vm_map *vm, unsigned long va);

/* Remove a 4 KB user page mapping and pmm_free the backing page.
 * Returns the freed PA, or 0 if unmapped. */
unsigned long mmu_vmap_unmap_user_4k(struct vm_map *vm, unsigned long va);
#endif

#endif

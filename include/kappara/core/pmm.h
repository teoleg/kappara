/*
 * include/kappara/pmm.h -- physical page allocator
 *
 * Vending machine for raw 4 KB pages of RAM.  All sized in PAGE_SIZE
 * chunks; no contiguous-N-pages allocator yet.  See kernel/pmm.c for
 * the freelist implementation and how the caller picks [start, end)
 * so that both the SoC peripheral window and the GPU's reserve stay
 * out of the freelist.
 */
#ifndef KAPPARA_PMM_H
#define KAPPARA_PMM_H

#include <stddef.h>

#define PAGE_SHIFT	12
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#define PAGE_MASK	(PAGE_SIZE - 1)

/*
 * Walks every page in [start, end), aligns the bounds to PAGE_SIZE,
 * and pushes each page onto the freelist.  Each arch's kmain passes
 * the bounds appropriate for its memory map -- typically
 * [__kernel_end .. start_of_MMIO_window).
 */
void   pmm_init(uintptr_t start, uintptr_t end);
/* Enrol an additional [start, end) range onto the freelist after
 * pmm_init has run.  Used by main.c to enrol non-contiguous chunks
 * so the user/exec VA windows (whose L2 entries are later overwritten
 * by mmu_map_user_2mb) stay out of the kernel-stack pool -- otherwise
 * a kernel stack allocated at one of those PAs aliases user-mode
 * storage and the saved-register frame gets clobbered by user writes. */
void   pmm_add_range(uintptr_t start, uintptr_t end);
void  *pmm_alloc(void);
void   pmm_free(void *p);
size_t pmm_free_count(void);

#endif

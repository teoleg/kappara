/*
 * kernel/pmm.c -- physical page allocator
 * =======================================
 *
 * What this file is
 * -----------------
 * The lowest layer of the memory manager: a free-list of 4 KB
 * physical pages.  Everyone who needs raw RAM (the slab allocator,
 * the scheduler for thread stacks, future page tables) ultimately
 * calls pmm_alloc() / pmm_free().
 *
 * The "next pointer inside the page" trick
 * ----------------------------------------
 * When a page is on the free list we have, by definition, no other
 * use for its contents.  So we steal its first 8 bytes to hold a
 * pointer to the next free page.  The free list is just a singly
 * linked stack:
 *
 *     freelist  --->  +---------+   +---------+   +---------+
 *                     | next    |-->| next    |-->| next=NUL|
 *                     | (rest   |   | (rest   |   | (rest   |
 *                     |  unused)|   |  unused)|   |  unused)|
 *                     +---------+   +---------+   +---------+
 *                     a 4 KB page   a 4 KB page   a 4 KB page
 *
 * No external bitmap, no per-page metadata, no fragmentation
 * concerns -- every page is interchangeable, and alloc/free are O(1)
 * pops/pushes.  The trade-off is that we can't ask "is this
 * particular page free?" without a linear scan, and we can't do
 * multi-page contiguous allocations.  That's fine for what we need
 * right now.
 *
 * What's on the free list
 * -----------------------
 * pmm_init walks every 4 KB page in:
 *
 *     [align_up(__kernel_end, 4K)  ..  align_down(PMM_LIMIT, 4K))
 *
 * and frees it.  __kernel_end comes from the linker script, just
 * past BSS and rounded up to a page.  PMM_LIMIT is the start of the
 * BCM2837 peripheral window (0x3F000000) -- everything above that
 * is MMIO, not RAM.  On a 1 GB Pi 3 that's about 257k free pages
 * (~1 GB) once the kernel image is removed.
 *
 * Why free high-to-low
 * --------------------
 * Because the freelist is LIFO, freeing in reverse address order
 * makes alloc() return the lowest addresses first -- which is purely
 * a cosmetic preference (the dumps in kmain look tidier) but free.
 *
 * Threading
 * ---------
 * No locking yet.  Until we have SMP, pmm is only ever called from
 * the kernel running on one core, and only ever with IRQs masked
 * around critical sections that don't currently exist.  When SMP
 * lands, freelist will need a spinlock or per-CPU caches.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/pmm.h"
#include "kappara/printk.h"

extern char __kernel_end[];

/*
 * Pi 3 / BCM2837: usable RAM ends where the peripheral window begins.
 * Everything above PMM_LIMIT is MMIO, mapped Device by the MMU.
 */
#define PMM_LIMIT	0x3F000000UL

static uintptr_t freelist;
static size_t    free_count;

static uintptr_t align_up(uintptr_t v, uintptr_t a) { return (v + a - 1) & ~(a - 1); }
static uintptr_t align_dn(uintptr_t v, uintptr_t a) { return v & ~(a - 1); }

static void zero_page(void *p)
{
	uint64_t *u = (uint64_t *)p;
	for (size_t i = 0; i < PAGE_SIZE / sizeof(*u); i++)
		u[i] = 0;
}

void pmm_free(void *p)
{
	uintptr_t pa = (uintptr_t)p;
	*(uintptr_t *)pa = freelist;
	freelist = pa;
	free_count++;
}

void *pmm_alloc(void)
{
	if (!freelist)
		return NULL;
	uintptr_t pa = freelist;
	freelist = *(uintptr_t *)pa;
	free_count--;
	zero_page((void *)pa);
	return (void *)pa;
}

size_t pmm_free_count(void) { return free_count; }

void pmm_init(void)
{
	uintptr_t start = align_up((uintptr_t)__kernel_end, PAGE_SIZE);
	uintptr_t end   = align_dn(PMM_LIMIT, PAGE_SIZE);

	/* Free low-to-high so allocations come back in ascending order. */
	for (uintptr_t pa = end; pa > start; ) {
		pa -= PAGE_SIZE;
		pmm_free((void *)pa);
	}

	kprintf("pmm: %lu pages free (%lu MiB) over [0x%lx..0x%lx)\n",
		(unsigned long)free_count,
		(unsigned long)((end - start) >> 20),
		(unsigned long)start, (unsigned long)end);
}

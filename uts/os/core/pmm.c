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
 *     [align_up(__kernel_end, 4K)  ..  align_down(end, 4K))
 *
 * and frees it.  __kernel_end comes from the linker script, just
 * past BSS and rounded up to a page.  The caller (kernel/main.c)
 * picks `end` so that two things stay out of the freelist:
 *
 *   - the SoC peripheral window (0x3F000000 on Pi 3, 0xFE000000 on
 *     Pi 4) -- that's MMIO, not RAM.
 *   - the GPU's reserved block, discovered at runtime by asking the
 *     firmware for a framebuffer and trimming `end` down to the FB
 *     PA.  Without this trim, kmalloc would eventually hand out a
 *     page the GPU is also using, and the resulting corruption is
 *     the classic "boots in QEMU, crashes on Pi" failure.
 *
 * On a 1 GB Pi 3 with the default ~64 MiB gpu_mem split that leaves
 * roughly 953 MiB of free RAM after kernel image + GPU reserve.
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
#include "kappara/spinlock.h"

static uintptr_t freelist;
static size_t    free_count;

/* Guards freelist + free_count.  Without this, two CPUs racing in
 * pmm_alloc can both pop the same head and end up handing the SAME
 * physical page out as two different kernel stacks (or slab pages),
 * which then trample each other's contents.  See commit history for
 * the "/proc/ps text in saved callee-saved regs" panic. */
static spinlock_t pmm_lock = SPINLOCK_INIT;

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
	if (!p) return;	/* defensive: never crash on free(NULL) */
	uintptr_t pa = (uintptr_t)p;
	unsigned long f = spin_lock_irq_save(&pmm_lock);
	*(uintptr_t *)pa = freelist;
	freelist = pa;
	free_count++;
	spin_unlock_irq_restore(&pmm_lock, f);
}

void *pmm_alloc(void)
{
	unsigned long f = spin_lock_irq_save(&pmm_lock);
	if (!freelist) {
		spin_unlock_irq_restore(&pmm_lock, f);
		return NULL;
	}
	uintptr_t pa = freelist;
	freelist = *(uintptr_t *)pa;
	free_count--;
	spin_unlock_irq_restore(&pmm_lock, f);
	zero_page((void *)pa);
	return (void *)pa;
}

size_t pmm_free_count(void) { return free_count; }

void pmm_add_range(uintptr_t start, uintptr_t end)
{
	start = align_up(start, PAGE_SIZE);
	end   = align_dn(end,   PAGE_SIZE);
	if (end <= start) return;

	/* Free high-to-low so allocations come back in ascending order. */
	for (uintptr_t pa = end; pa > start; ) {
		pa -= PAGE_SIZE;
		pmm_free((void *)pa);
	}

	kprintf("pmm: enrolled [0x%lx..0x%lx) (%lu MiB)\n",
		(unsigned long)start, (unsigned long)end,
		(unsigned long)((end - start) >> 20));
}

void pmm_init(uintptr_t start, uintptr_t end)
{
	pmm_add_range(start, end);
	kprintf("pmm: %lu pages free (%lu MiB) total\n",
		(unsigned long)free_count,
		(unsigned long)(free_count * PAGE_SIZE / (1024UL * 1024UL)));
}

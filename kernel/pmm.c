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

#ifndef KAPPARA_PMM_H
#define KAPPARA_PMM_H

#include <stddef.h>

#define PAGE_SHIFT	12
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#define PAGE_MASK	(PAGE_SIZE - 1)

void   pmm_init(void);
void  *pmm_alloc(void);
void   pmm_free(void *p);
size_t pmm_free_count(void);

#endif

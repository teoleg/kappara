#include <stdint.h>

#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/trap.h"
#include "kappara/uart.h"

static unsigned current_el(void)
{
	unsigned long el;
	__asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
	return (unsigned)((el >> 2) & 3u);
}

void kmain(void)
{
	uart_init();
	trap_init();

	kprintf("\nkappara: hello from aarch64\n");
	kprintf("        no soup for you, only streams\n");
	kprintf("        running at EL%u\n", current_el());

	mmu_init();
	pmm_init();

	uint64_t *p = pmm_alloc();
	if (p) {
		kprintf("pmm: alloc -> %p, %lu pages remain\n",
			(void *)p, (unsigned long)pmm_free_count());

		p[0] = 0xdeadbeefcafef00dUL;
		p[1] = (uintptr_t)p ^ 0xa5a5a5a5a5a5a5a5UL;
		kprintf("        wrote/read [0]=0x%lx [1]=0x%lx\n",
			(unsigned long)p[0], (unsigned long)p[1]);

		pmm_free(p);
		kprintf("        freed; %lu pages free\n",
			(unsigned long)pmm_free_count());

		uint64_t *q = pmm_alloc();
		kprintf("        re-alloc -> %p (should equal previous)\n", (void *)q);
	} else {
		kprintf("pmm: alloc returned NULL\n");
	}

	kpanic("end of kmain");
}

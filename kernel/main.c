#include <stdint.h>

#include "kappara/kmem.h"
#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/string.h"
#include "kappara/trap.h"
#include "kappara/uart.h"

static unsigned current_el(void)
{
	unsigned long el;
	__asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
	return (unsigned)((el >> 2) & 3u);
}

static void smoke_test_kmem(void)
{
	void *a = kmalloc(64);
	void *b = kmalloc(64);
	void *c = kmalloc(1024);

	kprintf("kmalloc: 64->%p  64->%p  1024->%p\n", a, b, c);
	kprintf("         (b-a)=%ld bytes  (expect 64)\n",
		(long)((char *)b - (char *)a));

	kmemset(a, 0xAA, 64);
	kmemcpy(b, a, 64);
	uint64_t *ub = b;
	kprintf("         memcpy ok: b[0]=0x%lx  b[7]=0x%lx\n",
		(unsigned long)ub[0], (unsigned long)ub[7]);

	kfree(a);
	kfree(b);
	kfree(c);

	void *a2 = kmalloc(64);
	kprintf("         re-alloc 64 -> %p  (expect %p back)\n", a2, b);
	kfree(a2);

	struct kmem_cache foo;
	kmem_cache_init(&foo, "foo", 48);
	void *o1 = kmem_cache_alloc(&foo);
	void *o2 = kmem_cache_alloc(&foo);
	void *o3 = kmem_cache_alloc(&foo);
	kprintf("foo cache: obj_size=%lu objs/slab=%lu  o1=%p o2=%p o3=%p\n",
		(unsigned long)foo.obj_size,
		(unsigned long)foo.objs_per_slab,
		o1, o2, o3);
	kprintf("         (o2-o1)=%ld  (o3-o2)=%ld  (expect 48 each)\n",
		(long)((char *)o2 - (char *)o1),
		(long)((char *)o3 - (char *)o2));
	kmem_cache_free(&foo, o2);
	kmem_cache_free(&foo, o3);
	kmem_cache_free(&foo, o1);
	kprintf("         foo: total=%lu free=%lu after frees\n",
		(unsigned long)foo.total_objs,
		(unsigned long)foo.free_objs);
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
	kmem_init();

	smoke_test_kmem();

	kpanic("end of kmain");
}

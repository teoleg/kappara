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

	kprintf("        firing brk #0 to test the trap path...\n");
	__asm__ volatile ("brk #0");

	kpanic("returned from brk?");
}

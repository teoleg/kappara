void uart_init(void);
void uart_puts(const char *s);

static unsigned current_el(void)
{
	unsigned long el;
	__asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
	return (unsigned)((el >> 2) & 3u);
}

void kmain(void)
{
	uart_init();
	uart_puts("\n");
	uart_puts("kappara: hello from aarch64\n");
	uart_puts("        no soup for you, only streams\n");

	switch (current_el()) {
	case 0: uart_puts("        running at EL0 (!)\n"); break;
	case 1: uart_puts("        running at EL1\n"); break;
	case 2: uart_puts("        running at EL2 (drop failed)\n"); break;
	case 3: uart_puts("        running at EL3 (drop failed)\n"); break;
	}

	for (;;)
		__asm__ volatile ("wfe");
}

void uart_init(void);
void uart_puts(const char *s);

void kmain(void)
{
	uart_init();
	uart_puts("\n");
	uart_puts("kappara: hello from aarch64\n");
	uart_puts("        no soup for you, only streams\n");
	for (;;)
		__asm__ volatile ("wfe");
}

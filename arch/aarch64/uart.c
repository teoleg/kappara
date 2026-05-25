#include <stdint.h>

#define PL011_BASE	0x3F201000UL

#define UART_DR		(PL011_BASE + 0x00)
#define UART_FR		(PL011_BASE + 0x18)
#define UART_IBRD	(PL011_BASE + 0x24)
#define UART_FBRD	(PL011_BASE + 0x28)
#define UART_LCRH	(PL011_BASE + 0x2C)
#define UART_CR		(PL011_BASE + 0x30)
#define UART_ICR	(PL011_BASE + 0x44)

#define FR_TXFF		(1u << 5)

#define LCRH_FEN	(1u << 4)
#define LCRH_WLEN_8	(3u << 5)

#define CR_UARTEN	(1u << 0)
#define CR_TXE		(1u << 8)
#define CR_RXE		(1u << 9)

static inline void mmio_write(uintptr_t reg, uint32_t v)
{
	*(volatile uint32_t *)reg = v;
}

static inline uint32_t mmio_read(uintptr_t reg)
{
	return *(volatile uint32_t *)reg;
}

void uart_init(void)
{
	mmio_write(UART_CR, 0);
	mmio_write(UART_ICR, 0x7FF);
	mmio_write(UART_IBRD, 26);
	mmio_write(UART_FBRD, 3);
	mmio_write(UART_LCRH, LCRH_FEN | LCRH_WLEN_8);
	mmio_write(UART_CR, CR_UARTEN | CR_TXE | CR_RXE);
}

void uart_putc(char c)
{
	while (mmio_read(UART_FR) & FR_TXFF) { }
	mmio_write(UART_DR, (uint32_t)(unsigned char)c);
}

void uart_puts(const char *s)
{
	while (*s) {
		if (*s == '\n')
			uart_putc('\r');
		uart_putc(*s++);
	}
}

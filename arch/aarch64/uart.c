/*
 * arch/aarch64/uart.c -- PL011 UART driver for Raspberry Pi 3 (BCM2837)
 * =====================================================================
 *
 * What this file is
 * -----------------
 * The minimum amount of code we need to talk to a serial console.  We
 * use ARM's PL011 PrimeCell UART -- there are two on the BCM2837
 * (PL011 and the simpler "mini UART"); the PL011 is at 0x3F201000 and
 * the easier one to program (real FIFOs, no MTU divisor games).
 *
 * Before the MMU is on, accesses to 0x3F201000 hit physical memory
 * directly with everything uncached.  After mmu_init() identity-maps
 * 0x3F000000..0x40000000 as Device-nGnRE, the same address still
 * works -- it's just travelling through translation and tagged as
 * MMIO so the CPU won't try to cache or reorder it.
 *
 * PL011 register map (offset from 0x3F201000)
 * -------------------------------------------
 *   0x00  DR      Data Register (read = received byte, write = TX byte)
 *   0x18  FR      Flag Register   (bit 5 = TXFF, transmit FIFO full)
 *   0x24  IBRD    Integer baud-rate divisor
 *   0x28  FBRD    Fractional baud-rate divisor
 *   0x2C  LCRH    Line Control: FIFO enable, word length, parity
 *   0x30  CR      Control: UART enable, TX enable, RX enable
 *   0x44  ICR     Interrupt Clear Register
 *
 * Baud rate
 * ---------
 * QEMU ignores the dividers; on real Pi 3 we want 115200 baud from a
 * 48 MHz UART clock:  divisor = 48e6 / (16 * 115200) = 26.0416...
 *   IBRD = 26   FBRD = round(0.0416 * 64) = 3
 *
 * Transmit
 * --------
 * Poll FR.TXFF until the TX FIFO has room, then write a byte to DR.
 * No interrupts, no DMA -- printf-style spinning is plenty for now.
 */

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

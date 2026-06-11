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

#include "kappara/core/spinlock.h"
#include "platform.h"

/*
 * Serialise UART access across CPUs.  Two output paths reach
 * uart_putc: kprintf (already serialised at kprintf-call level) and
 * the console-driver write path (sys_write -> STREAMS -> console
 * stream head -> uart_putc).  Without a UART-level lock the two
 * paths can interleave their characters when they happen on
 * different CPUs at the same time.
 *
 * We use uart_puts as the lock boundary: per-character locking is
 * way too fine-grained, per-puts gives a complete fragment.
 * Single-character uart_putc still locks per char because we don't
 * know how much the caller intends to emit; the caller typically
 * loops over uart_putc itself, so a per-char lock there is the
 * worst case.
 *
 * IRQ-save: the UART can be called from any context including IRQ
 * handlers (kprintf from inside an ISR is fair game).
 */
static spinlock_t uart_lock = SPINLOCK_INIT;

#define PL011_BASE	PLAT_PL011_BASE

#define UART_DR		(PL011_BASE + 0x00)
#define UART_FR		(PL011_BASE + 0x18)
#define UART_IBRD	(PL011_BASE + 0x24)
#define UART_FBRD	(PL011_BASE + 0x28)
#define UART_LCRH	(PL011_BASE + 0x2C)
#define UART_CR		(PL011_BASE + 0x30)
#define UART_ICR	(PL011_BASE + 0x44)

#define FR_TXFF		(1u << 5)
#define FR_RXFE		(1u << 4)

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

/*
 * Lock primitives, exposed so callers that emit a logical "line" of
 * output can grab the UART for the whole line and stream its bytes
 * via uart_putc_unlocked without each char paying the lock cost AND
 * without other CPUs sneaking bytes in between.
 *
 * kprintf does this around vkprintf; the console driver
 * (console_wq_putp) does it around each mblk it dispatches.
 */
unsigned long uart_acquire(void)
{
	return spin_lock_irq_save(&uart_lock);
}

void uart_release(unsigned long flags)
{
	spin_unlock_irq_restore(&uart_lock, flags);
}

void uart_putc_unlocked(char c)
{
	while (mmio_read(UART_FR) & FR_TXFF) { }
	mmio_write(UART_DR, (uint32_t)(unsigned char)c);
}

void uart_putc(char c)
{
	unsigned long f = uart_acquire();
	uart_putc_unlocked(c);
	uart_release(f);
}

void uart_puts(const char *s)
{
	unsigned long f = uart_acquire();
	while (*s) {
		if (*s == '\n')
			uart_putc_unlocked('\r');
		uart_putc_unlocked(*s++);
	}
	uart_release(f);
}

/* Lock-bypass send for kpanic.  If the UART lock is held by another
 * CPU that just died, the locked variant would deadlock and silence
 * the panic message -- the one thing it must never do.  IRQs stay
 * masked across the whole sequence so we don't interleave with our
 * own ISR. */
void uart_puts_panic(const char *s)
{
	unsigned long daif;
	__asm__ volatile ("mrs %0, daif" : "=r"(daif));
	__asm__ volatile ("msr daifset, #2" ::: "memory");
	while (*s) {
		if (*s == '\n')
			uart_putc_unlocked('\r');
		uart_putc_unlocked(*s++);
	}
	__asm__ volatile ("msr daif, %0" :: "r"(daif) : "memory");
}

int uart_getc_nonblock(void)
{
	if (mmio_read(UART_FR) & FR_RXFE)
		return -1;
	return (int)(mmio_read(UART_DR) & 0xFFu);
}

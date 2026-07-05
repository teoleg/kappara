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

/* Runtime UART base: PLAT_PL011_BASE on QEMU -kernel; overridden by
 * the SPCR address that efi_main discovers on UEFI / Nitro paths. */
extern uint64_t efi_uart_base;
extern uint32_t efi_uart_ibrd;
extern uint32_t efi_uart_fbrd;
extern uint8_t  efi_uart_type;   /* SPCR type: 0=16550, 3=PL011, 5=SBSA */
static uintptr_t pl011_base = PLAT_PL011_BASE;
static int uart_16550;           /* 1 when using 16550-compatible driver */

/* 16550 register offsets (byte-wide access).
 * ACPI SPCR type 0x00 = 16550-compatible; Nitro's virtualized serial
 * console is this type at the SPCR-reported address (0x090a0000). */
#define U16550_THR	0	/* TX holding register (DLAB=0) */
#define U16550_IER	1	/* interrupt enable */
#define U16550_FCR	2	/* FIFO control */
#define U16550_LCR	3	/* line control */
#define U16550_LSR	5	/* line status */
#define U16550_THRE	(1u << 5)	/* LSR: TX holding reg empty */

#define UART_DR		(pl011_base + 0x00)
#define UART_FR		(pl011_base + 0x18)
#define UART_IBRD	(pl011_base + 0x24)
#define UART_FBRD	(pl011_base + 0x28)
#define UART_LCRH	(pl011_base + 0x2C)
#define UART_CR		(pl011_base + 0x30)
#define UART_ICR	(pl011_base + 0x44)

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
	if (efi_uart_base != 0) {
		pl011_base = (uintptr_t)efi_uart_base;
		if (efi_uart_type == 0) {
			/*
			 * 16550-compatible path (SPCR type 0x00).
			 * AWS Nitro's virtualized serial console is this type
			 * at the SPCR address (0x090a0000).  The hypervisor
			 * manages baud rate transparently -- no divisor
			 * programming needed.  Minimal init: 8N1, FIFO on,
			 * no interrupts.
			 */
			uart_16550 = 1;
			volatile uint8_t *b =
				(volatile uint8_t *)(uintptr_t)efi_uart_base;
			b[U16550_IER] = 0x00;	/* no interrupts */
			b[U16550_LCR] = 0x03;	/* 8N1, DLAB=0 */
			b[U16550_FCR] = 0x07;	/* enable+clear FIFOs */
			return;
		}
		/*
		 * PL011 / SBSA path (type 3 or 5).
		 * Re-enable using saved or fallback baud-rate divisors.
		 * Older UEFI (Nitro 2018) writes CR=0 during EBS cleanup.
		 */
		uart_16550 = 0;
		mmio_write(UART_CR, 0);
		for (volatile unsigned i = 0; i < 1000000u; i++)
			if (!(mmio_read(UART_FR) & 0x08u)) break; /* !BUSY */
		mmio_write(UART_ICR, 0x7FFu);
		mmio_write(UART_IBRD, efi_uart_ibrd ? efi_uart_ibrd : 13u);
		mmio_write(UART_FBRD, efi_uart_fbrd ? efi_uart_fbrd : 1u);
		mmio_write(UART_LCRH, LCRH_FEN | LCRH_WLEN_8);
		mmio_write(UART_CR, CR_UARTEN | CR_TXE | CR_RXE);
		return;
	}
	/* QEMU -kernel path: full init at 115200/8N1 from scratch. */
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
	if (uart_16550) {
		volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)pl011_base;
		for (volatile unsigned i = 0; i < 1000000u; i++)
			if (b[U16550_LSR] & U16550_THRE) break;
		b[U16550_THR] = (uint8_t)c;
	} else {
		while (mmio_read(UART_FR) & FR_TXFF) { }
		mmio_write(UART_DR, (uint32_t)(unsigned char)c);
	}
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
	if (uart_16550) {
		volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)pl011_base;
		if (!(b[U16550_LSR] & 0x01u)) /* LSR.DR: data ready */
			return -1;
		return (int)b[U16550_THR];
	}
	if (mmio_read(UART_FR) & FR_RXFE)
		return -1;
	return (int)(mmio_read(UART_DR) & 0xFFu);
}

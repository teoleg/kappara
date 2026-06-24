/*
 * arch/aarch64/miniuart.c -- BCM2837 mini-UART (UART1) driver
 *
 * Companion to uart.c (PL011, our console).  The BCM2837 has two
 * UART blocks: PL011 at 0x3F201000 and the AUX/mini-UART at
 * 0x3F215040 (offset 0x40 inside the AUX block at 0x3F215000).  PL011
 * is the console; the mini-UART is the secondary serial port and is
 * where SLIP gets attached for off-host networking.
 *
 * QEMU's raspi3b model wires the second `-serial` slot to the AUX
 * mini-UART, so `-serial mon:stdio -serial pty` lets us host slip
 * connections from the controlling shell while the kappara console
 * stays on stdio.
 *
 * AUX register map (relative to 0x3F215000)
 * -----------------------------------------
 *   0x04  AUX_ENABLES   bit 0 = mini-UART enable
 *   0x40  AUX_MU_IO     TX FIFO (write) / RX FIFO (read), low 8 bits
 *   0x44  AUX_MU_IER    IRQ enable (we leave it 0 -- polling rx)
 *   0x48  AUX_MU_IIR    IRQ status / FIFO clear
 *   0x4C  AUX_MU_LCR    line control: 0x3 = 8-bit data (broadcom
 *                      erratum: both bits 0 and 1 must be set)
 *   0x50  AUX_MU_MCR    modem ctrl, leave 0
 *   0x54  AUX_MU_LSR    line status: bit 0 = RX ready,
 *                                    bit 5 = TX empty
 *   0x60  AUX_MU_CNTL   bit 0 = RX enable, bit 1 = TX enable
 *   0x68  AUX_MU_BAUD   baud divisor: (sysclk / (8 * baud)) - 1
 *
 * Polling rx -- no interrupt handler.  uts/os/net/slip.c spawns a
 * dedicated rx kthread that calls miniuart_rx_nonblock() in a tight
 * loop with kthread_yield() between empty reads.  Same shape PL011
 * uses today via uart_rx_main; an interrupt-driven UART rewrite is
 * orthogonal to this commit.
 */

#include <stdint.h>

#include "kappara/arch/miniuart.h"

#define AUX_BASE	0x3F215000UL

#define AUX_ENABLES	(AUX_BASE + 0x04)
#define AUX_MU_IO	(AUX_BASE + 0x40)
#define AUX_MU_IER	(AUX_BASE + 0x44)
#define AUX_MU_IIR	(AUX_BASE + 0x48)
#define AUX_MU_LCR	(AUX_BASE + 0x4C)
#define AUX_MU_MCR	(AUX_BASE + 0x50)
#define AUX_MU_LSR	(AUX_BASE + 0x54)
#define AUX_MU_CNTL	(AUX_BASE + 0x60)
#define AUX_MU_BAUD	(AUX_BASE + 0x68)

#define LSR_DR		(1u << 0)	/* RX data ready */
#define LSR_TXIDLE	(1u << 5)	/* TX FIFO can accept */

static inline void mmio_write(uintptr_t reg, uint32_t v)
{
	*(volatile uint32_t *)reg = v;
}

static inline uint32_t mmio_read(uintptr_t reg)
{
	return *(volatile uint32_t *)reg;
}

void miniuart_init(unsigned baud)
{
	/* Enable the mini-UART (without this, register accesses are
	 * undefined per the BCM datasheet).  Don't disturb other AUX
	 * bits in case SPI1/2 ever get wired up. */
	mmio_write(AUX_ENABLES, mmio_read(AUX_ENABLES) | 0x1);

	/* Disable rx/tx during config, no interrupts. */
	mmio_write(AUX_MU_CNTL, 0);
	mmio_write(AUX_MU_IER,  0);

	/* Clear and enable FIFOs (bits 1 and 2 = 1 to clear). */
	mmio_write(AUX_MU_IIR,  0xC6);

	/* 8-bit data.  Broadcom's mini-UART is documented incorrectly
	 * in the datasheet -- bit 0 = "data size" 0 means 7-bit, 1
	 * means 8-bit, but bit 1 ALSO needs to be set for 8-bit mode
	 * in practice.  Setting both = 0x3 is what Linux and u-boot
	 * do.  See https://elinux.org/BCM2835_datasheet_errata. */
	mmio_write(AUX_MU_LCR,  0x3);
	mmio_write(AUX_MU_MCR,  0);

	/* Baud divisor.  Assume 250 MHz core clock; QEMU ignores the
	 * register entirely so any sane value works there. */
	if (baud == 0) baud = 115200;
	unsigned divisor = (250000000u / (8u * baud)) - 1u;
	mmio_write(AUX_MU_BAUD, divisor);

	/* Enable rx + tx. */
	mmio_write(AUX_MU_CNTL, 0x3);
}

void miniuart_tx_byte(unsigned char c)
{
	while (!(mmio_read(AUX_MU_LSR) & LSR_TXIDLE)) { }
	mmio_write(AUX_MU_IO, (uint32_t)c);
}

int miniuart_rx_nonblock(void)
{
	if (!(mmio_read(AUX_MU_LSR) & LSR_DR))
		return -1;
	return (int)(mmio_read(AUX_MU_IO) & 0xffu);
}

/*
 * include/kappara/miniuart.h -- BCM2837 mini-UART (UART1) driver.
 *
 * Sits alongside PL011 (uart.h) at the hardware level: PL011 is our
 * console UART (0x3F201000), the mini-UART is the secondary serial
 * port (0x3F215000 +0x40 for the MU registers).  On real Pi 3
 * hardware the mini-UART is what's typically connected to GPIO14/15
 * for an external serial peripheral once PL011 is taken by the
 * console.  In QEMU's raspi3b model the AUX block is emulated; the
 * second -serial slot lands on it.
 *
 * Used by uts/os/net/slip.c as the byte-stream driver under the SLIP
 * STREAMS module.  Polling rx via a kthread (no interrupts yet),
 * matching the shape uart_rx_main uses for PL011.
 */

#ifndef KAPPARA_MINIUART_H
#define KAPPARA_MINIUART_H

void miniuart_init(unsigned baud);
void miniuart_tx_byte(unsigned char c);

/* Non-blocking read: returns the byte (0..255) or -1 if the rx FIFO
 * is empty.  The kthread polls this and yields between empties. */
int  miniuart_rx_nonblock(void);

#endif

/*
 * include/kappara/uart.h -- early console UART API
 *
 * The contract every arch's uart.c must satisfy.  Used by printk.c
 * (which never speaks to MMIO directly) and by each arch's kmain to
 * bring the console up before anything else.
 *
 *   uart_init           bring up the device
 *   uart_putc           transmit one byte (busy-poll on TX FIFO)
 *   uart_getc_nonblock  return next RX byte or -1 if FIFO empty
 */
#ifndef KAPPARA_UART_H
#define KAPPARA_UART_H

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
int  uart_getc_nonblock(void);	/* -1 if RX FIFO is empty */

#endif

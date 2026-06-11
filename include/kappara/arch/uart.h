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
void uart_putc(char c);			/* locked single-byte send */
void uart_puts(const char *s);		/* locked NUL-terminated send */
void uart_puts_panic(const char *s);	/* lock-bypass send, for kpanic */
int  uart_getc_nonblock(void);		/* -1 if RX FIFO is empty */

/* Lock-then-stream variants for callers that emit many bytes in a
 * loop and want them to land contiguously without other CPUs slipping
 * bytes between.  Pattern:
 *
 *     unsigned long f = uart_acquire();
 *     while (...) uart_putc_unlocked(c);
 *     uart_release(f);
 */
unsigned long uart_acquire(void);
void          uart_release(unsigned long flags);
void          uart_putc_unlocked(char c);

#endif

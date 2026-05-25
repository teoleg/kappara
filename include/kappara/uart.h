/*
 * include/kappara/uart.h -- early console UART API
 *
 * The contract every arch's uart.c must satisfy.  Used by printk.c
 * (which never speaks to MMIO directly) and by each arch's kmain to
 * bring the console up before anything else.
 */
#ifndef KAPPARA_UART_H
#define KAPPARA_UART_H

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);

#endif

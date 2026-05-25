#ifndef KAPPARA_PRINTK_H
#define KAPPARA_PRINTK_H

#include <stdarg.h>

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vkprintf(const char *fmt, va_list ap);
void kpanic(const char *msg) __attribute__((noreturn));

#endif

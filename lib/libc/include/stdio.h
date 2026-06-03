#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
int printf  (const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int sprintf (char *buf,             const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t n,   const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vprintf (const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);
#endif /* _STDIO_H */

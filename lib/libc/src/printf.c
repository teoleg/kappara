#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "../aarch64/internal.h"

/*
 * vsnprintf -- core formatting engine.
 *
 * Writes at most n-1 characters to buf and always null-terminates.
 * Returns the number of characters that would have been written had
 * the buffer been large enough (C99 semantics).
 *
 * Supported conversions:
 *   %s  %d  %i  %u  %x  %X  %c  %p  %%
 * Modifiers: l (long), width, zero-pad (0), left-align (-)
 */
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    size_t pos   = 0;   /* chars written so far (excluding NUL) */
    size_t total = 0;   /* chars that would have been written     */

#define EMIT(ch) do {                          \
        if (pos + 1 < n) buf[pos++] = (ch);   \
        total++;                               \
    } while (0)

    while (*fmt) {
        if (*fmt != '%') {
            EMIT(*fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Flags */
        int left_align = 0;
        int zero_pad   = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0') zero_pad   = 1;
            fmt++;
        }
        if (left_align) zero_pad = 0; /* left-align overrides zero-pad */

        /* Width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        /* Length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        char   tmp[32];
        char  *str   = (char *)0;
        size_t slen  = 0;
        char   padc  = zero_pad ? '0' : ' ';

        switch (*fmt++) {
        case 's': {
            str = va_arg(ap, char *);
            if (!str) str = (char *)"(null)";
            slen = strlen(str);
            /* string: left-pad with spaces only, never with zeros */
            padc = ' ';
            break;
        }
        case 'd':
        case 'i': {
            long val = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            int  neg = (val < 0);
            unsigned long uval = neg ? (unsigned long)(-val) : (unsigned long)val;
            int i = 0;
            if (uval == 0) { tmp[i++] = '0'; }
            else { while (uval) { tmp[i++] = (char)('0' + uval % 10); uval /= 10; } }
            if (neg) tmp[i++] = '-';
            /* reverse */
            for (int a = 0, b = i - 1; a < b; a++, b--) {
                char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t;
            }
            tmp[i] = '\0';
            str  = tmp;
            slen = (size_t)i;
            break;
        }
        case 'u': {
            unsigned long val = is_long ? va_arg(ap, unsigned long)
                                        : (unsigned long)va_arg(ap, unsigned int);
            int i = 0;
            if (val == 0) { tmp[i++] = '0'; }
            else { while (val) { tmp[i++] = (char)('0' + val % 10); val /= 10; } }
            for (int a = 0, b = i - 1; a < b; a++, b--) {
                char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t;
            }
            tmp[i] = '\0';
            str  = tmp;
            slen = (size_t)i;
            break;
        }
        case 'x':
        case 'X': {
            unsigned long val = is_long ? va_arg(ap, unsigned long)
                                        : (unsigned long)va_arg(ap, unsigned int);
            const char *digits = (fmt[-1] == 'X') ? "0123456789ABCDEF"
                                                   : "0123456789abcdef";
            int i = 0;
            if (val == 0) { tmp[i++] = '0'; }
            else { while (val) { tmp[i++] = digits[val & 0xf]; val >>= 4; } }
            for (int a = 0, b = i - 1; a < b; a++, b--) {
                char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t;
            }
            tmp[i] = '\0';
            str  = tmp;
            slen = (size_t)i;
            break;
        }
        case 'c': {
            tmp[0] = (char)va_arg(ap, int);
            tmp[1] = '\0';
            str  = tmp;
            slen = 1;
            break;
        }
        case 'p': {
            /* pointer: "0x" + 16 lowercase hex digits */
            unsigned long val = (unsigned long)(unsigned long)va_arg(ap, void *);
            tmp[0] = '0'; tmp[1] = 'x';
            for (int i = 0; i < 16; i++)
                tmp[2 + i] = "0123456789abcdef"[(val >> (60 - i * 4)) & 0xf];
            tmp[18] = '\0';
            str  = tmp;
            slen = 18;
            /* pointers are never zero-padded beyond the fixed hex */
            width = 0;
            break;
        }
        case '%': {
            EMIT('%');
            continue;
        }
        default:
            /* Unknown specifier — emit as-is */
            EMIT('%');
            EMIT(fmt[-1]);
            continue;
        }

        /* Pad and emit */
        if (str) {
            int pad = (width > 0 && (size_t)width > slen)
                      ? (int)((size_t)width - slen) : 0;

            if (!left_align)
                for (int i = 0; i < pad; i++) EMIT(padc);

            for (size_t i = 0; i < slen; i++) EMIT(str[i]);

            if (left_align)
                for (int i = 0; i < pad; i++) EMIT(' ');
        }
    }

    if (n > 0)
        buf[pos] = '\0';

    return (int)total;
#undef EMIT
}

int vprintf(const char *fmt, va_list ap)
{
    char buf[1024];
    int  len = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len > 0) {
        size_t out = (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1;
        __syscall3(__NR_write, 1, (long)(unsigned long)buf, (long)out);
    }
    return len;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    /* Use a very large limit; caller is responsible for buffer size */
    int r = vsnprintf(buf, (size_t)0x7fffffff, fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s)
{
    size_t len = strlen(s);
    __syscall3(__NR_write, 1, (long)(unsigned long)s, (long)len);
    char nl = '\n';
    __syscall3(__NR_write, 1, (long)(unsigned long)&nl, 1);
    return (int)(len + 1);
}

int putchar(int c)
{
    char ch = (char)c;
    __syscall3(__NR_write, 1, (long)(unsigned long)&ch, 1);
    return c;
}

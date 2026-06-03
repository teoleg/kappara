#include <stdlib.h>
#include "../aarch64/internal.h"

void _exit(int status)
{
    __syscall1(__NR_exit, (long)status);
    __builtin_unreachable();
}

void exit(int status)
{
    _exit(status);
}

/*
 * strtol -- convert string to long.
 * Handles base 0 (auto-detect: 0x = hex, 0 = octal, else decimal),
 * and explicit bases 2-36.
 */
long strtol(const char *s, char **endp, int base)
{
    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        s++;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    /* Auto-detect base */
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
            s++;
        } else {
            base = 10;
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    long val = 0;
    const char *start = s;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9')      digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        val = val * base + digit;
        s++;
    }

    if (endp)
        *endp = (char *)(s == start ? start : s);

    return neg ? -val : val;
}

int atoi(const char *s)
{
    return (int)strtol(s, (char **)0, 10);
}

long atol(const char *s)
{
    return strtol(s, (char **)0, 10);
}

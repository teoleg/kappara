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

/* atof is intentionally absent: kappara userland builds with
 * -mgeneral-regs-only, which forbids `double` return types.  Code
 * that needs string->number should use strtol/atoi instead. */

int  abs (int n)  { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }

/* No env in kappara -- userland is a single static shell.  Returning
 * NULL is the correct contract: ISO C says getenv returns NULL when
 * the name isn't bound, and nothing is bound here. */
char *getenv(const char *name)
{
    (void)name;
    return (char *)0;
}

/* qsort -- insertion sort.  Fine for the smallish arrays cmd tools
 * pass (ls directory entries, ftp file lists).  O(n^2) but no heap
 * required for stack-frame recursion, no extra allocation. */
void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *))
{
    unsigned char *b = (unsigned char *)base;
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0; j--) {
            unsigned char *a = b + (j - 1) * size;
            unsigned char *c = b + j * size;
            if (cmp(a, c) <= 0)
                break;
            for (size_t k = 0; k < size; k++) {
                unsigned char t = a[k];
                a[k] = c[k];
                c[k] = t;
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *))
{
    const unsigned char *b = (const unsigned char *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void *p = b + mid * size;
        int r = cmp(key, p);
        if (r == 0)
            return (void *)p;
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return (void *)0;
}

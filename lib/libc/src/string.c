#include <string.h>
#include <stdlib.h>

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && *a == *b) {
        a++;
        b++;
    }
    if (n == (size_t)-1)
        return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && (*d++ = *src++))
        n--;
    while (n--)
        *d++ = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (*d)
        d++;
    while (n-- && (*d = *src++)) {
        d++;
    }
    *d = '\0';
    return dst;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c)
            return (char *)s;
    if (c == '\0')
        return (char *)s;
    return (char *)0;
}

char *strrchr(const char *s, int c)
{
    const char *last = (const char *)0;
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c)
            last = s;
    if (c == '\0')
        return (char *)s;
    return (char *)last;
}

char *strstr(const char *hay, const char *needle)
{
    if (!*needle)
        return (char *)hay;
    size_t nlen = strlen(needle);
    for (; *hay; hay++) {
        if (*hay == *needle && !strncmp(hay, needle, nlen))
            return (char *)hay;
    }
    return (char *)0;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) return (char *)0;
    memcpy(p, s, n);
    return p;
}

/* strtok keeps a static cursor between calls; not thread-safe but
 * matches the POSIX shape libc users expect. */
char *strtok(char *s, const char *delim)
{
    static char *save;
    if (s)
        save = s;
    if (!save)
        return (char *)0;

    while (*save && strchr(delim, (unsigned char)*save))
        save++;
    if (!*save) {
        save = (char *)0;
        return (char *)0;
    }

    char *tok = save;
    while (*save && !strchr(delim, (unsigned char)*save))
        save++;
    if (*save) {
        *save++ = '\0';
    } else {
        save = (char *)0;
    }
    return tok;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c)
            return (void *)p;
        p++;
    }
    return (void *)0;
}

/*
 * include/kappara/string.h -- byte-copy primitives
 *
 * kmemset and kmemcpy.  Naming with the k- prefix keeps them out of
 * libc's namespace -- we explicitly do not link libc, but the
 * compiler still recognises memset/memcpy as builtin pattern targets
 * and we don't want any accidental aliasing.  See kernel/string.c.
 */
#ifndef KAPPARA_STRING_H
#define KAPPARA_STRING_H

#include <stddef.h>

void  *kmemset(void *dst, int c, size_t n);
void  *kmemcpy(void *dst, const void *src, size_t n);
int    kmemcmp(const void *a, const void *b, size_t n);
int    kstrcmp(const char *a, const char *b);
size_t kstrlen(const char *s);

#endif

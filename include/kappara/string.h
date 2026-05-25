#ifndef KAPPARA_STRING_H
#define KAPPARA_STRING_H

#include <stddef.h>

void *kmemset(void *dst, int c, size_t n);
void *kmemcpy(void *dst, const void *src, size_t n);

#endif

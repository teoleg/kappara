#include "kappara/string.h"

void *kmemset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;
	unsigned char v = (unsigned char)c;
	while (n--)
		*d++ = v;
	return dst;
}

void *kmemcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}

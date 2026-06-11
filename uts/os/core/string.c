/*
 * kernel/string.c -- byte-wise mem* helpers
 * =========================================
 *
 * Just kmemset and kmemcpy.  Trivial, byte-at-a-time, no alignment
 * tricks -- the kernel runs cold enough that we don't care yet.
 *
 * Why exist separately at all?  Because freestanding C doesn't give us
 * a memset/memcpy, and some compiler optimisations (loop-to-memset
 * pattern rewrites) actually expect to *find* one and emit a call to
 * "memset".  We provide our own with k* names so we control the
 * implementation; if the compiler ever inserts a real memset call we
 * can `__attribute__((alias("kmemset")))` here in one place.
 */

#include "kappara/core/string.h"

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

int kmemcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a;
	const unsigned char *q = b;
	while (n--) {
		if (*p != *q) return (int)*p - (int)*q;
		p++; q++;
	}
	return 0;
}

int kstrcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

size_t kstrlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

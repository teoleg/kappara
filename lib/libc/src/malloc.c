/*
 * lib/libc/src/malloc.c -- free-list allocator backed by sys_brk
 *
 * The heap lives at EXEC_HEAP_VA (0x20400000), a 2 MB window the
 * kernel maps at exec_space_init time.  sys_brk(0) returns the
 * current break; sys_brk(addr) sets it.  We maintain a singly-
 * linked free list and split/coalesce in the usual way.
 *
 * sbrk(n) is implemented inline: ask the kernel for current break,
 * bump it by n, return the old break as the start of new memory.
 *
 * Block layout:
 *   [ size | free | *next ] <-- block_t header (24 bytes on AArch64)
 *   [ user data            ]
 *
 * The 'size' field is user-payload bytes, not including the header.
 */

#include <stdlib.h>
#include <string.h>
#include "../aarch64/internal.h"

typedef struct block {
	unsigned long  size;   /* user-payload bytes */
	int            free;
	struct block  *next;
} block_t;

static block_t *freelist;

/* Ask kernel for more heap.  Returns start of new region on success,
 * NULL on failure.  Size is rounded up to 4 KB to amortise syscalls. */
static void *heap_grow(unsigned long n)
{
	n = (n + 0xfffUL) & ~0xfffUL;	/* round up to 4 KB */
	long cur = __syscall1(__NR_brk, 0);
	if (cur < 0) return (void *)0;
	long nb  = __syscall1(__NR_brk, cur + (long)n);
	if (nb < 0 || nb == cur) return (void *)0;
	return (void *)(unsigned long)cur;
}

void *malloc(size_t size)
{
	if (size == 0) return (void *)0;
	size = (size + 7) & ~(size_t)7;	/* 8-byte align */

	/* First fit in existing free list. */
	block_t *b = freelist;
	while (b) {
		if (b->free && b->size >= size) {
			if (b->size >= size + sizeof(block_t) + 8) {
				block_t *nb = (block_t *)((char *)(b + 1) + size);
				nb->size = b->size - size - sizeof(block_t);
				nb->free = 1;
				nb->next = b->next;
				b->size  = size;
				b->next  = nb;
			}
			b->free = 0;
			return (void *)(b + 1);
		}
		b = b->next;
	}

	/* No fit -- grow the heap. */
	unsigned long need = size + sizeof(block_t);
	void *raw = heap_grow(need);
	if (!raw) return (void *)0;

	block_t *nb = (block_t *)raw;
	nb->size = size;
	nb->free = 0;
	nb->next = (void *)0;

	/* Append to free list. */
	if (!freelist) {
		freelist = nb;
	} else {
		block_t *t = freelist;
		while (t->next) t = t->next;
		t->next = nb;
	}
	return (void *)(nb + 1);
}

void free(void *ptr)
{
	if (!ptr) return;
	block_t *b = (block_t *)ptr - 1;
	b->free = 1;
	if (b->next && b->next->free) {
		b->size += sizeof(block_t) + b->next->size;
		b->next  = b->next->next;
	}
}

void *calloc(size_t nmemb, size_t size)
{
	size_t total = nmemb * size;
	void *p = malloc(total);
	if (p) memset(p, 0, total);
	return p;
}

void *realloc(void *ptr, size_t size)
{
	if (!ptr)  return malloc(size);
	if (!size) { free(ptr); return (void *)0; }
	block_t *b = (block_t *)ptr - 1;
	if (b->size >= size) return ptr;
	void *np = malloc(size);
	if (np) { memcpy(np, ptr, b->size); free(ptr); }
	return np;
}

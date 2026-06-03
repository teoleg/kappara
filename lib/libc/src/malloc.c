#include <stdlib.h>
#include <string.h>

#define HEAP_SIZE (512 * 1024)

typedef struct block {
    size_t        size;   /* bytes of user data following this header */
    int           free;
    struct block *next;
} block_t;

static unsigned char _heap[HEAP_SIZE];
static block_t      *_freelist;

static void heap_init(void) {
    _freelist = (block_t *)_heap;
    _freelist->size = HEAP_SIZE - sizeof(block_t);
    _freelist->free = 1;
    _freelist->next = NULL;
}

void *malloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 7) & ~(size_t)7;   /* 8-byte align */

    if (!_freelist) heap_init();

    block_t *b = _freelist;
    while (b) {
        if (b->free && b->size >= size) {
            /* split if enough room for a new header + >=8 bytes */
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
    return NULL; /* OOM */
}

void free(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)ptr - 1;
    b->free = 1;
    /* coalesce with next */
    if (b->next && b->next->free) {
        b->size += sizeof(block_t) + b->next->size;
        b->next  = b->next->next;
    }
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr)  return malloc(size);
    if (!size) { free(ptr); return NULL; }
    block_t *b = (block_t *)ptr - 1;
    if (b->size >= size) return ptr;
    void *np = malloc(size);
    if (np) { memcpy(np, ptr, b->size); free(ptr); }
    return np;
}

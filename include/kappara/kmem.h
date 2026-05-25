#ifndef KAPPARA_KMEM_H
#define KAPPARA_KMEM_H

#include <stddef.h>

struct slab;

struct kmem_cache {
	const char  *name;
	size_t       obj_size;
	size_t       objs_per_slab;
	struct slab *slabs;
	size_t       total_objs;
	size_t       free_objs;
};

void  kmem_init(void);

void  kmem_cache_init(struct kmem_cache *c, const char *name, size_t obj_size);
void *kmem_cache_alloc(struct kmem_cache *c);
void  kmem_cache_free(struct kmem_cache *c, void *p);

void *kmalloc(size_t size);
void  kfree(void *p);

#endif

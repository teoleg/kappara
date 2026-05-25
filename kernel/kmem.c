#include <stddef.h>
#include <stdint.h>

#include "kappara/kmem.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/string.h"

#define SLAB_MAGIC	0x5BAB10C0u
#define OBJ_ALIGN	8u

/*
 * One slab == one 4 KB page.  Header sits at the start of the page so that
 * given any object pointer we can recover the slab (and therefore the cache)
 * by masking to the page boundary.  kfree(p) needs no size argument.
 */
struct slab {
	uint32_t            magic;
	uint32_t            free_count;
	struct slab        *next;
	struct kmem_cache  *cache;
	void               *freelist;
};

static size_t slab_header_size(void)
{
	return (sizeof(struct slab) + OBJ_ALIGN - 1) & ~(OBJ_ALIGN - 1);
}

static size_t objs_per_slab_for(size_t obj_size)
{
	size_t available = PAGE_SIZE - slab_header_size();
	return available / obj_size;
}

static struct slab *slab_of(void *p)
{
	return (struct slab *)((uintptr_t)p & ~PAGE_MASK);
}

static struct slab *grow_cache(struct kmem_cache *c)
{
	struct slab *s = pmm_alloc();
	if (!s)
		return NULL;

	s->magic      = SLAB_MAGIC;
	s->cache      = c;
	s->next       = c->slabs;
	s->free_count = c->objs_per_slab;
	s->freelist   = NULL;

	uintptr_t obj_base = (uintptr_t)s + slab_header_size();
	for (size_t i = c->objs_per_slab; i-- > 0; ) {
		void *obj = (void *)(obj_base + i * c->obj_size);
		*(void **)obj = s->freelist;
		s->freelist = obj;
	}

	c->slabs       = s;
	c->total_objs += c->objs_per_slab;
	c->free_objs  += c->objs_per_slab;
	return s;
}

void kmem_cache_init(struct kmem_cache *c, const char *name, size_t obj_size)
{
	if (obj_size < sizeof(void *))
		obj_size = sizeof(void *);
	obj_size = (obj_size + OBJ_ALIGN - 1) & ~(OBJ_ALIGN - 1);

	c->name          = name;
	c->obj_size      = obj_size;
	c->objs_per_slab = objs_per_slab_for(obj_size);
	c->slabs         = NULL;
	c->total_objs    = 0;
	c->free_objs     = 0;
}

void *kmem_cache_alloc(struct kmem_cache *c)
{
	struct slab *s;
	for (s = c->slabs; s; s = s->next)
		if (s->free_count > 0)
			break;
	if (!s) {
		s = grow_cache(c);
		if (!s)
			return NULL;
	}

	void *p = s->freelist;
	s->freelist = *(void **)p;
	s->free_count--;
	c->free_objs--;
	return p;
}

static void slab_free(struct slab *s, void *p)
{
	*(void **)p = s->freelist;
	s->freelist = p;
	s->free_count++;
	s->cache->free_objs++;
}

void kmem_cache_free(struct kmem_cache *c, void *p)
{
	if (!p)
		return;
	struct slab *s = slab_of(p);
	if (s->magic != SLAB_MAGIC || s->cache != c) {
		kprintf("kmem_cache_free(%s, %p): bad slab (magic=0x%x cache=%p)\n",
			c->name, p, s->magic, (void *)s->cache);
		return;
	}
	slab_free(s, p);
}

void kfree(void *p)
{
	if (!p)
		return;
	struct slab *s = slab_of(p);
	if (s->magic != SLAB_MAGIC) {
		kprintf("kfree(%p): not a slab page (magic=0x%x)\n",
			p, s->magic);
		return;
	}
	slab_free(s, p);
}

/* ---- kmalloc: power-of-two size caches ----------------------------------- */

static struct {
	const char        *name;
	size_t             size;
	struct kmem_cache  cache;
} size_caches[] = {
	{ .name = "size-16",   .size = 16   },
	{ .name = "size-32",   .size = 32   },
	{ .name = "size-64",   .size = 64   },
	{ .name = "size-128",  .size = 128  },
	{ .name = "size-256",  .size = 256  },
	{ .name = "size-512",  .size = 512  },
	{ .name = "size-1024", .size = 1024 },
	{ .name = "size-2048", .size = 2048 },
};

#define N_SIZE_CACHES (sizeof(size_caches) / sizeof(size_caches[0]))

void kmem_init(void)
{
	for (size_t i = 0; i < N_SIZE_CACHES; i++)
		kmem_cache_init(&size_caches[i].cache,
				size_caches[i].name,
				size_caches[i].size);

	kprintf("kmem: %lu size caches, slab_header=%lu bytes\n",
		(unsigned long)N_SIZE_CACHES,
		(unsigned long)slab_header_size());
}

void *kmalloc(size_t size)
{
	if (size == 0)
		return NULL;
	for (size_t i = 0; i < N_SIZE_CACHES; i++) {
		if (size_caches[i].size >= size)
			return kmem_cache_alloc(&size_caches[i].cache);
	}
	kprintf("kmalloc(%lu): too large\n", (unsigned long)size);
	return NULL;
}

#ifndef _DLFCN_H
#define _DLFCN_H

/* DYNAMIC.md stage 7: minimal dlfcn surface.
 *
 *   void *dlopen(const char *path, int mode)
 *     -- Load the shared object at `path`.  `mode` is currently
 *        ignored (no RTLD_LAZY / RTLD_GLOBAL semantics).  Returns
 *        an opaque handle or NULL on failure.
 *
 *   void *dlsym(void *handle, const char *name)
 *     -- Look up `name` in the object identified by `handle`.
 *        Returns the symbol's runtime address or NULL.
 *
 *   int dlclose(void *handle)
 *     -- Not implemented yet; returns 0 unconditionally.  Stage 8.
 *
 *   const char *dlerror(void)
 *     -- Not implemented; returns NULL.
 */

#define RTLD_LAZY    0x1
#define RTLD_NOW     0x2
#define RTLD_GLOBAL  0x100
#define RTLD_LOCAL   0x000

void *dlopen (const char *path, int mode);
void *dlsym  (void *handle, const char *name);
int   dlclose(void *handle);
const char *dlerror(void);

#endif /* _DLFCN_H */

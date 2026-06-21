#include <dlfcn.h>
#include "../aarch64/internal.h"

void *dlopen(const char *path, int mode)
{
    (void)mode;	/* ignored: kernel always eager-loads + applies RELATIVE */
    long h = __syscall1(__NR_dlopen, (long)(unsigned long)path);
    return (void *)(unsigned long)h;
}

void *dlsym(void *handle, const char *name)
{
    long r = __syscall3(__NR_dlsym, (long)(unsigned long)handle,
                                    (long)(unsigned long)name, 0);
    return (void *)(unsigned long)r;
}

int dlclose(void *handle)
{
    (void)handle;	/* stage 8 */
    return 0;
}

const char *dlerror(void)
{
    return (const char *)0;
}

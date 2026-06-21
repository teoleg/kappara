#include <dlfcn.h>
#include "../aarch64/internal.h"

/* Per-process dlerror buffer.  POSIX says dlerror returns NULL when no
 * error is pending and that the buffer's content is valid until the
 * next dlerror call.  We poll the kernel via SYS_dlerror, which clears
 * the kernel-side message after copying it here. */
static char dlerror_buf[128];
static int  dlerror_have = 0;

static void dlerror_refresh(void)
{
    long n = __syscall3(__NR_dlerror, (long)(unsigned long)dlerror_buf,
                        (long)sizeof(dlerror_buf), 0);
    dlerror_have = (n > 0);
}

void *dlopen(const char *path, int mode)
{
    (void)mode;	/* ignored: kernel always eager-loads + applies RELATIVE */
    long h = __syscall1(__NR_dlopen, (long)(unsigned long)path);
    if (h == 0) dlerror_refresh();
    return (void *)(unsigned long)h;
}

void *dlsym(void *handle, const char *name)
{
    long r = __syscall3(__NR_dlsym, (long)(unsigned long)handle,
                                    (long)(unsigned long)name, 0);
    if (r == 0) dlerror_refresh();
    return (void *)(unsigned long)r;
}

int dlclose(void *handle)
{
    long r = __syscall1(__NR_dlclose, (long)(unsigned long)handle);
    if (r != 0) dlerror_refresh();
    return (int)r;
}

const char *dlerror(void)
{
    if (!dlerror_have) return (const char *)0;
    dlerror_have = 0;	/* POSIX: subsequent calls return NULL */
    return dlerror_buf;
}

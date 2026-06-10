#include <unistd.h>
#include "../aarch64/internal.h"

ssize_t write(int fd, const void *buf, size_t n)
{
    return (ssize_t)__syscall3(__NR_write, (long)fd,
                               (long)(unsigned long)buf, (long)n);
}

ssize_t read(int fd, void *buf, size_t n)
{
    return (ssize_t)__syscall3(__NR_read, (long)fd,
                               (long)(unsigned long)buf, (long)n);
}

int open(const char *path, int flags)
{
    /* open uses a 2-arg syscall; use __syscall3 with flags in x1 */
    register long x0 __asm__("x0") = (long)(unsigned long)path;
    register long x1 __asm__("x1") = (long)flags;
    register long x8 __asm__("x8") = __NR_open;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
    return (int)x0;
}

int close(int fd)
{
    return (int)__syscall1(__NR_close, (long)fd);
}

pid_t getpid(void)
{
    return (pid_t)__syscall1(__NR_getpid, 0);
}

pid_t fork(void)
{
    return (pid_t)__syscall0(__NR_fork);
}

int wait(int tid)
{
    return (int)__syscall1(__NR_wait, (long)tid);
}

int pipe(int fds[2])
{
    return (int)__syscall1(__NR_pipe, (long)(unsigned long)fds);
}
/* _exit lives in stdlib.c -- shared by exit() and the direct
 * _exit() callers.  Don't duplicate it here. */

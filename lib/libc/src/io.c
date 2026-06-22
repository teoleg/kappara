#include <stropts.h>
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

int putmsg(int fd, const struct strbuf *ctl,
           const struct strbuf *data, int flags)
{
    return (int)__syscall4(__NR_putmsg, (long)fd,
                           (long)(unsigned long)ctl,
                           (long)(unsigned long)data,
                           (long)flags);
}

int getmsg(int fd, struct strbuf *ctl,
           struct strbuf *data, int *flagsp)
{
    return (int)__syscall4(__NR_getmsg, (long)fd,
                           (long)(unsigned long)ctl,
                           (long)(unsigned long)data,
                           (long)(unsigned long)flagsp);
}

int ioctl(int fd, int cmd, long arg)
{
    return (int)__syscall3(__NR_ioctl, (long)fd, (long)cmd, arg);
}

int creat(const char *path)
{
    return (int)__syscall1(__NR_creat, (long)(unsigned long)path);
}

int mkdir(const char *path)
{
    return (int)__syscall1(__NR_mkdir, (long)(unsigned long)path);
}

int rmdir(const char *path)
{
    return (int)__syscall1(__NR_rmdir, (long)(unsigned long)path);
}

int unlink(const char *path)
{
    return (int)__syscall1(__NR_unlink, (long)(unsigned long)path);
}

long lseek(int fd, long off, int whence)
{
    return __syscall3(__NR_seek, (long)fd, off, (long)whence);
}

long ls(const char *path, char *out, size_t cap)
{
    return __syscall3(__NR_ls, (long)(unsigned long)path,
                                (long)(unsigned long)out, (long)cap);
}

long ll(const char *path, char *out, size_t cap)
{
    return __syscall3(__NR_ll, (long)(unsigned long)path,
                                (long)(unsigned long)out, (long)cap);
}

int execve(const char *path, const char *const argv[])
{
    return (int)__syscall3(__NR_execve, (long)(unsigned long)path,
                                         (long)(unsigned long)argv, 0);
}

int chdir(const char *path)
{
    return (int)__syscall1(__NR_chdir, (long)(unsigned long)path);
}

char *getcwd(char *buf, size_t cap)
{
    long n = __syscall3(__NR_getcwd, (long)(unsigned long)buf, (long)cap, 0);
    return (n < 0) ? (char *)0 : buf;
}
/* _exit lives in stdlib.c -- shared by exit() and the direct
 * _exit() callers.  Don't duplicate it here. */

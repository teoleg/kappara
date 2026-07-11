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

int dup(int oldfd)
{
    return (int)__syscall1(__NR_dup, (long)oldfd);
}

int dup2(int oldfd, int newfd)
{
    return (int)__syscall2(__NR_dup2, (long)oldfd, (long)newfd);
}

#include <sys/mman.h>

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, long offset)
{
    long r = __syscall6(__NR_mmap, (long)(unsigned long)addr,
                        (long)length, (long)prot, (long)flags,
                        (long)fd, (long)offset);
    if (r < 0) return MAP_FAILED;
    return (void *)(unsigned long)r;
}

int munmap(void *addr, size_t length)
{
    return (int)__syscall2(__NR_munmap, (long)(unsigned long)addr,
                           (long)length);
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

/* Dump a file to stdout in 1 KB chunks.  When strip_preamble is set,
 * lines beginning with "# " are dropped -- that's the format the
 * kernel /proc entries use for their plain-language explanatory
 * header, so `<cmd> -` gives the bare data for scripts / experts.
 * Returns 0 on success, -1 if the file could not be opened. */
int proc_cat(const char *path, int strip_preamble)
{
    int fd = open(path, 0);
    if (fd < 0) return -1;
    char buf[1024];
    int  at_bol = 1;      /* at beginning of a line */
    int  skipping = 0;    /* currently inside a dropped "# ..." line */
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        long start = 0;
        for (long i = 0; i < n; i++) {
            if (at_bol) {
                /* Decide this line's fate from its first char.  A
                 * "# " line is dropped only when stripping. */
                skipping = strip_preamble && buf[i] == '#';
                at_bol = 0;
                if (skipping) start = i + 1; /* skip from here */
            }
            if (buf[i] == '\n') {
                if (!skipping)
                    write(1, buf + start, (size_t)(i - start + 1));
                start = i + 1;
                at_bol = 1;
                skipping = 0;
            }
        }
        if (!skipping && start < n)
            write(1, buf + start, (size_t)(n - start));
    }
    close(fd);
    return 0;
}

#ifndef _UNISTD_H
#define _UNISTD_H
#include <sys/types.h>
ssize_t write(int fd, const void *buf, size_t n);
ssize_t read (int fd, void       *buf, size_t n);
int     open (const char *path, int flags);
int     close(int fd);
pid_t   getpid(void);
pid_t   fork (void);
int     wait (int tid);
int     pipe (int fds[2]);
int     creat(const char *path);
int     mkdir(const char *path);
int     rmdir(const char *path);
int     unlink(const char *path);
long    lseek(int fd, long off, int whence);
long    ls   (const char *path, char *out, size_t cap);
long    ll   (const char *path, char *out, size_t cap);
int     execve(const char *path, const char *const argv[]);
void   _exit (int status) __attribute__((noreturn));
#endif /* _UNISTD_H */

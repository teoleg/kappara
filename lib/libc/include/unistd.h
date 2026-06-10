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
void   _exit (int status) __attribute__((noreturn));
#endif /* _UNISTD_H */

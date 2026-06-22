#ifndef _ERRNO_H
#define _ERRNO_H

/* Single global errno -- TLS comes later (R_AARCH64_TLSDESC).
 * The kappara syscall ABI doesn't actually set this yet; libc
 * callers can assign to it and read it back, but nothing in the
 * kernel populates it.  Stage 1 just makes the declaration real
 * so compiling code that mentions `errno` doesn't error out. */
extern int errno;

#define EPERM       1
#define ENOENT      2
#define ESRCH       3
#define EINTR       4
#define EIO         5
#define ENXIO       6
#define E2BIG       7
#define ENOEXEC     8
#define EBADF       9
#define ECHILD     10
#define EAGAIN     11
#define ENOMEM     12
#define EACCES     13
#define EFAULT     14
#define EBUSY      16
#define EEXIST     17
#define EXDEV      18
#define ENODEV     19
#define ENOTDIR    20
#define EISDIR     21
#define EINVAL     22
#define ENFILE     23
#define EMFILE     24
#define ENOTTY     25
#define EFBIG      27
#define ENOSPC     28
#define ESPIPE     29
#define EROFS      30
#define EMLINK     31
#define EPIPE      32
#define ERANGE     34
#define ENAMETOOLONG 36
#define ENOSYS     38
#define ENOTEMPTY  39
#define ECONNRESET 104
#define ECONNREFUSED 111

#define EWOULDBLOCK EAGAIN

#endif /* _ERRNO_H */

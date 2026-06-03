#ifndef LIBC_INTERNAL_H
#define LIBC_INTERNAL_H
#include <sys/types.h>

/* Syscall numbers — must match include/kappara/syscall.h */
#define __NR_getpid      1
#define __NR_open        3
#define __NR_close       4
#define __NR_read        5
#define __NR_write       6
#define __NR_pipe        11
#define __NR_exit        16
#define __NR_kill        19
#define __NR_sigaction   22
#define __NR_sigprocmask 24
#define __NR_sigsuspend  25

static inline long __syscall1(long nr, long a0) {
    register long x0 __asm__("x0") = a0;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long __syscall3(long nr, long a0, long a1, long a2) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return x0;
}

#endif /* LIBC_INTERNAL_H */

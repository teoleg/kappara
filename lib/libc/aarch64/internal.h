#ifndef LIBC_INTERNAL_H
#define LIBC_INTERNAL_H
#include <sys/types.h>

/* Syscall numbers — must match include/kappara/syscall.h */
#define __NR_getpid      1
#define __NR_open        3
#define __NR_close       4
#define __NR_read        5
#define __NR_write       6
#define __NR_putmsg      8
#define __NR_getmsg      9
#define __NR_ioctl       7
#define __NR_ls          10
#define __NR_pipe        11
#define __NR_creat       12
#define __NR_seek        13
#define __NR_mkdir       14
#define __NR_exit        16
#define __NR_unlink      17
#define __NR_rmdir       18
#define __NR_kill        19
#define __NR_ll          20
#define __NR_sigaction   22
#define __NR_sigprocmask 24
#define __NR_sigsuspend  25
#define __NR_wait        26
#define __NR_execve      27
#define __NR_brk         33
#define __NR_fork        34
#define __NR_clock_gettime 35
#define __NR_dlopen      36
#define __NR_dlsym       37
#define __NR_dlclose     38
#define __NR_dlerror     39
#define __NR_chdir       40
#define __NR_getcwd      41
#define __NR_dup         42
#define __NR_dup2        43

static inline long __syscall0(long nr) {
    register long x0 __asm__("x0");
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long __syscall1(long nr, long a0) {
    register long x0 __asm__("x0") = a0;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long __syscall2(long nr, long a0, long a1) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
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

static inline long __syscall4(long nr, long a0, long a1, long a2, long a3) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                     : "memory", "cc");
    return x0;
}

#endif /* LIBC_INTERNAL_H */

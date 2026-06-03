#include <signal.h>
#include "../aarch64/internal.h"

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    register long x0 __asm__("x0") = (long)sig;
    register long x1 __asm__("x1") = (long)(unsigned long)act;
    register long x2 __asm__("x2") = (long)(unsigned long)oldact;
    register long x8 __asm__("x8") = __NR_sigaction;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8)
                     : "memory", "cc");
    return (int)x0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    register long x0 __asm__("x0") = (long)how;
    register long x1 __asm__("x1") = (long)(unsigned long)set;
    register long x2 __asm__("x2") = (long)(unsigned long)oldset;
    register long x8 __asm__("x8") = __NR_sigprocmask;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8)
                     : "memory", "cc");
    return (int)x0;
}

int sigsuspend(sigset_t mask)
{
    return (int)__syscall1(__NR_sigsuspend, (long)mask);
}

int kill(pid_t pid, int sig)
{
    register long x0 __asm__("x0") = (long)pid;
    register long x1 __asm__("x1") = (long)sig;
    register long x8 __asm__("x8") = __NR_kill;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8)
                     : "memory", "cc");
    return (int)x0;
}

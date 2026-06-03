#include <stdio.h>
#include "../user/syscall.h"

static volatile int marker;

static void handler(int sig)
{
    (void)sig;
    puts("masktest: handler ran (delivered via sigsuspend)");
    marker = 1;
}

int main(void)
{
    struct sigaction sa, old_sa;
    sa.sa_handler = handler; sa.sa_mask = 0; sa.sa_flags = 0;
    if (sys_sigaction(SIGTERM, &sa, &old_sa) < 0) {
        puts("masktest: sigaction failed");
        return 1;
    }
    unsigned int block = 1u << (SIGTERM - 1), old_mask = 0;
    sys_sigprocmask(SIG_BLOCK, &block, &old_mask);
    puts("masktest: SIGTERM blocked; sending to self");
    marker = 0;
    sys_kill((int)sys_getpid(), SIGTERM);
    puts("masktest: still here (signal pended); calling sigsuspend");
    sys_sigsuspend(old_mask);
    printf("masktest: sigsuspend returned, marker=%d\n", marker);
    sys_sigaction(SIGTERM, &old_sa, 0);
    sys_sigprocmask(SIG_SETMASK, &old_mask, 0);
    return 0;
}

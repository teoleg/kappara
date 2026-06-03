#include <stdio.h>
#include "../user/syscall.h"

static volatile int marker;

static void handler(int sig)
{
    printf("sigtest: handler running, sig=%d\n", sig);
    marker = 0xC0DE;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct sigaction sa, old;
    sa.sa_handler = handler; sa.sa_mask = 0; sa.sa_flags = 0;
    if (sys_sigaction(SIGTERM, &sa, &old) < 0) {
        puts("sigtest: sigaction failed");
        return 1;
    }
    puts("sigtest: handler installed; sending SIGTERM to self");
    marker = 0;
    sys_kill((int)sys_getpid(), SIGTERM);
    printf("sigtest: back after signal, marker=0x%x\n", (unsigned int)marker);
    struct sigaction restore;
    restore.sa_handler = SIG_DFL_PTR; restore.sa_mask = 0; restore.sa_flags = 0;
    sys_sigaction(SIGTERM, &restore, 0);
    return 0;
}

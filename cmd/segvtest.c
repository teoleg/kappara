#include <stdio.h>
#include "../user/syscall.h"

static void segv_handler(int sig)
{
    (void)sig;
    sys_log("segvtest: handler caught SIGSEGV; exiting");
    sys_exit();
}

static void worker(long arg)
{
    (void)arg;
    struct sigaction sa;
    sa.sa_handler = segv_handler; sa.sa_mask = 0; sa.sa_flags = 0;
    sys_sigaction(SIGSEGV, &sa, 0);
    sys_log("segvtest: about to deref NULL");
    volatile int *q = (volatile int *)0;
    *q = 1;
    sys_log("segvtest: BUG -- continued past fault");
    sys_exit();
}

int main(void)
{
    long tid = sys_spawn(worker, 0);
    if (tid < 0) { puts("segvtest: spawn failed"); return 1; }
    printf("segvtest: spawned tid=%ld\n", tid);
    sys_wait((int)tid);
    puts("segvtest: worker exited");
    return 0;
}

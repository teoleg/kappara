#include <stdio.h>
#include "../user/syscall.h"

static void worker(long arg)
{
    (void)arg;
    for (int i = 0; i < 20000; i++) sys_yield();
    sys_exit();
}

int main(void)
{
    long tid = sys_spawn(worker, 0);
    if (tid < 0) { puts("waittest: spawn failed"); return 1; }
    printf("waittest: spawned tid=%ld, waiting...\n", tid);
    long r = sys_wait((int)tid);
    printf("waittest: sys_wait returned %ld (0 = clean)\n", r);
    return 0;
}

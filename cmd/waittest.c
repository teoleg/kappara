#include <stdio.h>
#include "../user/syscall.h"

static void worker(long arg)
{
    sys_exit((int)arg);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Spawn worker with exit status 42; verify wait returns 42. */
    long tid = sys_spawn(worker, 42);
    if (tid < 0) { puts("waittest: spawn failed\n"); return 1; }
    printf("waittest: spawned tid=%ld, waiting...\n", tid);
    long r = sys_wait((int)tid);
    if (r != 42) {
        printf("waittest: FAIL expected 42 got %ld\n", r);
        return 1;
    }

    /* Zero exit status. */
    tid = sys_spawn(worker, 0);
    if (tid < 0) { puts("waittest: spawn failed (2)\n"); return 1; }
    r = sys_wait((int)tid);
    if (r != 0) {
        printf("waittest: FAIL expected 0 got %ld\n", r);
        return 1;
    }

    puts("waittest: PASS\n");
    return 0;
}

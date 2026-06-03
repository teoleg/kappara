#include "ulib.h"
static void worker(long arg) {
    (void)arg;
    for (int i = 0; i < 20000; i++) sys_yield();
    sys_exit();
}
void _start(void) {
    long tid = sys_spawn(worker, 0);
    if (tid < 0) { err("waittest: spawn failed\r\n"); sys_exit(); }
    out("waittest: spawned tid="); out_long(tid); out(", waiting...\r\n");
    long r = sys_wait((int)tid);
    out("waittest: sys_wait returned "); out_long(r); out(" (0 = clean)\r\n");
    sys_exit();
}

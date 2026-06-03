#include "ulib.h"
static volatile int marker;
static void handler(int sig) {
    out("sigtest: handler running, sig="); out_long(sig); out("\r\n");
    marker = 0xC0DE;
}
void _start(void) {
    struct sigaction sa, old;
    sa.sa_handler = handler; sa.sa_mask = 0; sa.sa_flags = 0;
    if (sys_sigaction(SIGTERM, &sa, &old) < 0) { err("sigtest: sigaction failed\r\n"); sys_exit(); }
    out("sigtest: handler installed; sending SIGTERM to self\r\n");
    marker = 0;
    sys_kill((int)sys_getpid(), SIGTERM);
    out("sigtest: back after signal, marker=0x"); out_hex((unsigned long)marker); out("\r\n");
    struct sigaction restore; restore.sa_handler = SIG_DFL_PTR; restore.sa_mask = 0; restore.sa_flags = 0;
    sys_sigaction(SIGTERM, &restore, 0);
    sys_exit();
}

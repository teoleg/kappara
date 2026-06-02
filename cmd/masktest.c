#include "ulib.h"
static volatile int marker;
static void handler(int sig) { (void)sig; out("masktest: handler ran (delivered via sigsuspend)\r\n"); marker = 1; }
void _start(void) {
    struct sigaction sa, old_sa;
    sa.sa_handler = handler; sa.sa_mask = 0; sa.sa_flags = 0;
    if (sys_sigaction(SIGTERM, &sa, &old_sa) < 0) { err("masktest: sigaction failed\r\n"); sys_exit(); }
    unsigned int block = 1u << (SIGTERM - 1);
    unsigned int old_mask = 0;
    sys_sigprocmask(SIG_BLOCK, &block, &old_mask);
    out("masktest: SIGTERM blocked; sending to self\r\n");
    marker = 0;
    sys_kill((int)sys_getpid(), SIGTERM);
    out("masktest: still here (signal pended); calling sigsuspend\r\n");
    sys_sigsuspend(old_mask);
    out("masktest: sigsuspend returned, marker="); out_long(marker); out("\r\n");
    sys_sigaction(SIGTERM, &old_sa, 0);
    sys_sigprocmask(SIG_SETMASK, &old_mask, 0);
    sys_exit();
}

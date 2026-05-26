/*
 * include/kappara/ksh.h -- kappara shell entry point
 *
 * Started as a kthread from kmain: kthread_create("ksh", ksh_main, NULL).
 * Reads lines from the UART and dispatches each verb to the syscall
 * layer.  Implementation + command list in kernel/ksh.c.
 */
#ifndef KAPPARA_KSH_H
#define KAPPARA_KSH_H

void ksh_main(void *arg);

#endif

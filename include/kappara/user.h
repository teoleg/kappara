/*
 * include/kappara/user.h -- AArch64 userspace bring-up
 *
 * Tiny first-cut user mode: one 2 MB region remapped to be EL0-RW
 * via mmu_map_user_2mb, populated with a hand-coded user program
 * that issues a single svc #SYS_log and then halts in EL0.
 *
 *   user_init   -- copy program bytes + message into the user
 *                  backing storage, remap the region, do I-cache
 *                  maintenance.  Called from kmain after mmu_init.
 *
 *   user_spawn  -- create a kthread which, on first run, eret's
 *                  into EL0 at the user entry point.  Once it does,
 *                  the thread leaves EL1 entirely until the user
 *                  re-enters via svc.
 */
#ifndef KAPPARA_USER_H
#define KAPPARA_USER_H

void user_init(void);
void user_spawn(void);

#endif

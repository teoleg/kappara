/*
 * include/kappara/uaccess.h -- user/kernel pointer safety helpers
 *
 * Before this layer existed, a syscall like sys_log(ptr) would just
 * kprintf("%s", ptr) -- the kernel ran at EL1 with full TTBR0 access
 * and would happily dereference whatever address the user passed.
 * That meant a user process could leak arbitrary kernel memory via
 * sys_log((char *)0x80000), or MMIO peripherals at 0x3F000000, etc.
 *
 * This header gives the kernel a way to say "this pointer came from
 * user mode, refuse it unless it points inside the user-accessible
 * window".  Today that window is the single 2 MB user region
 * (USER_VA .. USER_VA + USER_SIZE) we map in kernel/user.c; when we
 * grow to per-process address spaces this will become a per-task
 * predicate.
 *
 *   syscall_from_user   set by the arch trap dispatcher (1 on SVC
 *                       from EL0/USR, 0 on SVC from EL1/SVC).
 *
 *   user_ptr_ok(p,n)    p..p+n lies inside the user window.
 *
 *   copy_from_user      bounds-check + byte copy from user buffer
 *                       into a kernel buffer.
 *   copy_to_user        the reverse.
 *
 *   strncpy_from_user   copy a NUL-terminated string out of user
 *                       memory, byte-checking each one as we go.
 *                       Returns length on success, -1 on bad ptr,
 *                       -1 if no NUL appears within maxlen.
 *
 * For kernel-mode SVC callers (ARMv7's in-kernel ksh) the dispatcher
 * leaves syscall_from_user=0 and the handlers fall back to direct
 * dereference -- they're trusted.
 */

#ifndef KAPPARA_UACCESS_H
#define KAPPARA_UACCESS_H

#include <stddef.h>

/* Set/cleared by each arch's trap_dispatch around the SVC handler. */
extern int syscall_from_user;

int  user_ptr_ok(const void *p, size_t n);

long copy_from_user(void *dst, const void *usrc, size_t n);
long copy_to_user(void *udst, const void *src, size_t n);
long strncpy_from_user(char *dst, const char *usrc, size_t maxlen);

#endif

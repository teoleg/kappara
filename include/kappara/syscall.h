/*
 * include/kappara/syscall.h -- syscall numbers and dispatcher
 *
 * Calling convention (matches the trap-frame layout each arch saves):
 *
 *   AArch64    number  in x8
 *              args    in x0..x5
 *              return  in x0
 *              trap    svc #0   (lands at sync current/lower EL vector,
 *                                ESR_EL1.EC == 0x15)
 *
 *   ARMv7      number  in r7
 *              args    in r0..r5
 *              return  in r0
 *              trap    svc #0   (lands at SVC vector, vec_id == 2)
 *
 * Today every call originates in the kernel (we do not have EL0/USR
 * mode yet) so the "user" pointer in sys_log et al. is a kernel
 * pointer.  When userspace lands, syscall_dispatch picks up
 * copy_from_user / copy_to_user wrappers for the ones that take
 * pointers and the calling convention above is unchanged.
 */
#ifndef KAPPARA_SYSCALL_H
#define KAPPARA_SYSCALL_H

#define SYS_log		0	/* (const char *msg)                       */
#define SYS_getpid	1	/* (void)                                  */
#define SYS_yield	2	/* (void)                                  */
#define SYS_open	3	/* (const char *name)             -> fd    */
#define SYS_close	4	/* (int fd)                                */
#define SYS_read	5	/* (int fd, void *buf, size_t len) -> n    */
#define SYS_write	6	/* (int fd, const void *buf, size_t len)   */
#define SYS_ioctl	7	/* (int fd, int cmd, long arg)             */

#define SYS_MAX		8

long syscall_dispatch(long num, long a0, long a1, long a2,
		      long a3, long a4, long a5);

#endif

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
#define SYS_putmsg	8	/* (int fd, strbuf *c, strbuf *d, int fl)  */
#define SYS_getmsg	9	/* (int fd, strbuf *c, strbuf *d, int *fl) */
#define SYS_ls		10	/* (const char *path, char *out, size_t cap) */
#define SYS_pipe	11	/* (int fds[2]) -> 0 / -1                   */
#define SYS_creat	12	/* (const char *path) -> 0 / -1             */
#define SYS_seek	13	/* (int fd, long off, int whence) -> newpos */
#define SYS_mkdir	14	/* (const char *path) -> 0 / -1             */
#define SYS_spawn	15	/* (void (*entry)(long), long arg) -> tid   */
#define SYS_exit	16	/* (void)  no return                        */
#define SYS_unlink	17	/* (const char *path) -> 0 / -1             */
#define SYS_rmdir	18	/* (const char *path) -> 0 / -1             */
#define SYS_kill	19	/* (int tid, int sig) -> 0 / -1             */
#define SYS_ll		20	/* same args as SYS_ls but emits "type     */
				/* size name\n" rows (ls -l style)         */
#define SYS_halt	21	/* ARM semihosting SYS_EXIT -- asks QEMU   */
				/* to terminate cleanly (no return)        */
#define SYS_sigaction	22	/* (int sig, const sigaction *act,         *
				 *  sigaction *oldact) -> 0/-1            */
#define SYS_sigreturn	23	/* (void) -- restores sigframe; only       *
				 * the kernel-built trampoline issues it   */
#define SYS_sigprocmask	24	/* (int how, const uint32_t *set,          *
				 *  uint32_t *oldset) -> 0/-1             */
#define SYS_sigsuspend	25	/* (uint32_t mask) -> -1 (after handler)   */
#define SYS_wait	26	/* (int tid) -> 0 once tid has exited,     *
				 * -1 on bad tid or interruption          */
#define SYS_execve	27	/* (const char *path) -> tid / -1          */
#define SYS_setpgid	28	/* (pid, pgid) -- POSIX setpgid             */
#define SYS_getpgrp	29	/* () -> curthread->t_pgrp                  */
#define SYS_setsid	30	/* () -> new session id                     */
#define SYS_tcsetpgrp	31	/* (fd, pgid) -> 0 -- set tty fg pgrp       */
#define SYS_tcgetpgrp	32	/* (fd) -> pgid                             */
#define SYS_brk		33	/* (uintptr_t addr) -> new break / -1      */
#define SYS_fork	34	/* () -> child tid (parent) / 0 (child)    *
				 * v0: child shares parent's vm_map, gets  *
				 * its own stack slot with parent's stack  *
				 * copied.  See docs/ARCHITECTURE.md.      */
#define SYS_clock_gettime 35	/* (int clk_id, struct timespec *ts) -> 0/-1 *
				 * clk_id is currently ignored; we always   *
				 * return CNTPCT-derived monotonic time.    */
#define SYS_dlopen	36	/* (const char *path) -> handle / 0       *
				 * DYNAMIC.md stage 7: load a shared object  *
				 * into the calling process, apply its own  *
				 * RELATIVE relocs, return its load base.   */
#define SYS_dlsym	37	/* (handle, const char *name) -> addr / 0  *
				 * Look up `name` in the shared object's    *
				 * dynsym; return the resolved runtime VA   *
				 * or 0 if absent.                          */
#define SYS_dlclose	38	/* (handle) -> 0 / -1                       *
				 * Unmap the slot's pages, release the      *
				 * handle.                                  */
#define SYS_dlerror	39	/* (char *buf, size_t cap) -> bytes / 0    *
				 * Copy the most recent error string and   *
				 * clear it.                                */

#define SYS_MAX		40

long syscall_dispatch(long num, long a0, long a1, long a2,
		      long a3, long a4, long a5);

#endif

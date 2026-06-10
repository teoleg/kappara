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
 *
 *   exec_space_init -- map the exec VA windows (0x20000000 code,
 *                  0x20200000 stack) and register /bin blobs in VFS.
 *                  Called from kmain after user_init.
 *
 *   sys_execve_impl -- kernel-side execve: load an ELF from VFS,
 *                  copy segments into exec_storage, spawn a new
 *                  kthread that ertes into the ELF entry point.
 *                  Returns the new tid, or -1 on error.
 */
#ifndef KAPPARA_USER_H
#define KAPPARA_USER_H

#include <stdint.h>

void user_init(void);
void user_spawn(void);
void exec_space_init(void);

/* sys_spawn -- create a new user-mode thread that shares the address
 * space with init.  `entry` must lie inside the user 2 MB region;
 * the new thread starts at EL0 with x0 = arg and SP_EL0 pointing
 * into its own 64 KB stack slot.  Returns tid on success, -1. */
long sys_spawn_impl(uint64_t entry, uint64_t arg);

/* sys_execve -- load an ELF from `path`, map it into the exec VA
 * window, set up the exec stack with argc/argv, spawn a new thread,
 * inherit fds from the caller.
 * Returns the new thread's tid, or -1 on failure. */
long sys_execve_impl(const char *path, int argc, const char *const argv[]);

/* sys_exit -- terminate the calling thread with exit status.  Does not return. */
void sys_exit_impl(int status) __attribute__((noreturn));

/* sys_brk -- set the heap break for the current exec session.
 * addr == 0: return current break.
 * addr in [EXEC_HEAP_VA, EXEC_HEAP_VA+EXEC_HEAP_SIZE]: set break,
 *   return new break.
 * Out-of-range: return -1. */
long sys_brk_impl(uint64_t addr);

/* Release an exec slot back to the pool.  Called by vm_map_put when
 * the last reference to a vm_map bound to that slot drops. */
void exec_slot_release(int slot);

/* sys_fork -- R5.  Duplicates the calling exec'd process into a new
 * one with its own physical backing for EXEC_VA/EXEC_STACK_VA/
 * EXEC_HEAP_VA.  The child sees the parent's state at fork time;
 * subsequent writes don't cross the boundary because both processes
 * have their own vm_map mapping the same EL0 VAs to different
 * physical pages.
 *
 * Returns child tid in the parent, 0 in the child, -1 on error
 * (no free exec slot, vm_map_create failed, etc.).
 *
 * Only callable from an exec'd process (EXEC_VA range).  Init-shell
 * threads sharing USER_VA can't fork -- they have no per-process
 * physical backing. */
struct trap_frame;
long sys_fork_impl(struct trap_frame *parent_tf);

#endif


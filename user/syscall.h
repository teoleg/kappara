/*
 * user/syscall.h -- syscall wrappers for the user side
 *
 * Mirror image of include/kappara/syscall.h's table for SYS_* numbers
 * (we duplicate the numbers here so user code doesn't pull in kernel
 * headers).  Each wrapper is a small inline that pins arguments into
 * x0..x5 and the number into x8 the way the kernel trap dispatcher
 * expects, then issues svc #0.
 */

#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#define SYS_log		0
#define SYS_getpid	1
#define SYS_yield	2
#define SYS_open	3
#define SYS_close	4
#define SYS_read	5
#define SYS_write	6
#define SYS_ioctl	7
#define SYS_putmsg	8
#define SYS_getmsg	9
#define SYS_ls		10
#define SYS_pipe	11
#define SYS_creat	12
#define SYS_seek	13
#define SYS_mkdir	14
#define SYS_spawn	15
#define SYS_exit	16
#define SYS_unlink	17
#define SYS_rmdir	18

#define SEEK_SET	0
#define SEEK_CUR	1
#define SEEK_END	2

#define O_TRUNC		0x01

typedef long ssize_t;
typedef unsigned long size_t;

static inline long _syscall1(long num, long a0)
{
	register long x0 __asm__("x0") = a0;
	register long x8 __asm__("x8") = num;
	__asm__ volatile ("svc #0"
			  : "+r"(x0) : "r"(x8) : "memory", "cc");
	return x0;
}

static inline long _syscall3(long num, long a0, long a1, long a2)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x8 __asm__("x8") = num;
	__asm__ volatile ("svc #0"
			  : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8)
			  : "memory", "cc");
	return x0;
}

static inline long sys_log(const char *msg)
{
	return _syscall1(SYS_log, (long)(unsigned long)msg);
}

static inline long sys_getpid(void)
{
	return _syscall1(SYS_getpid, 0);
}

static inline long sys_yield(void)
{
	return _syscall1(SYS_yield, 0);
}

static inline long sys_write(int fd, const void *buf, size_t n)
{
	return _syscall3(SYS_write, fd, (long)(unsigned long)buf, (long)n);
}

static inline long sys_open(const char *path, int flags)
{
	register long x0 __asm__("x0") = (long)(unsigned long)path;
	register long x1 __asm__("x1") = (long)flags;
	register long x8 __asm__("x8") = SYS_open;
	__asm__ volatile ("svc #0"
			  : "+r"(x0) : "r"(x1), "r"(x8)
			  : "memory", "cc");
	return x0;
}

static inline long sys_mkdir(const char *path)
{
	return _syscall1(SYS_mkdir, (long)(unsigned long)path);
}

static inline long sys_spawn(void (*entry)(long), long arg)
{
	register long x0 __asm__("x0") = (long)(unsigned long)entry;
	register long x1 __asm__("x1") = arg;
	register long x8 __asm__("x8") = SYS_spawn;
	__asm__ volatile ("svc #0"
			  : "+r"(x0) : "r"(x1), "r"(x8)
			  : "memory", "cc");
	return x0;
}

static inline void sys_exit(void) __attribute__((noreturn));
static inline void sys_exit(void)
{
	register long x8 __asm__("x8") = SYS_exit;
	__asm__ volatile ("svc #0" :: "r"(x8) : "memory");
	__builtin_unreachable();
}

static inline long sys_unlink(const char *path)
{
	return _syscall1(SYS_unlink, (long)(unsigned long)path);
}

static inline long sys_rmdir(const char *path)
{
	return _syscall1(SYS_rmdir, (long)(unsigned long)path);
}

static inline long sys_close(int fd)
{
	return _syscall1(SYS_close, fd);
}

static inline long sys_read(int fd, void *buf, size_t n)
{
	return _syscall3(SYS_read, fd, (long)(unsigned long)buf, (long)n);
}

static inline long sys_ioctl(int fd, int cmd, long arg)
{
	return _syscall3(SYS_ioctl, fd, cmd, arg);
}

static inline long sys_ls(const char *path, char *out, size_t cap)
{
	return _syscall3(SYS_ls,
			 (long)(unsigned long)path,
			 (long)(unsigned long)out,
			 (long)cap);
}

static inline long sys_pipe(int fds[2])
{
	return _syscall1(SYS_pipe, (long)(unsigned long)fds);
}

static inline long sys_creat(const char *path)
{
	return _syscall1(SYS_creat, (long)(unsigned long)path);
}

static inline long sys_seek(int fd, long offset, int whence)
{
	return _syscall3(SYS_seek, fd, offset, whence);
}

/* ioctl commands -- mirror include/kappara/stream_head.h. */
#define I_PUSH		1
#define I_POP		2
#define I_LIST		3

#endif

/*
 * kernel/user.c -- AArch64 EL0 userspace, first cut
 * =================================================
 *
 * What this file is
 * -----------------
 * The minimum machinery to get a thread running at EL0 (user mode)
 * and back into the kernel via SVC.  Once it works, ksh + future
 * processes can move "out of the kernel" -- but for this first cut
 * we just want to prove the boundary itself.
 *
 * Outline
 * -------
 *
 *   user_init()                              (from kmain, after mmu_init)
 *       |
 *       v
 *   copy user_program + user_msg into user_storage (kernel-VA write)
 *   ic iallu + dsb + isb                     (instructions become visible)
 *   mmu_map_user_2mb(USER_VA, &user_storage) (publish to EL0)
 *
 *   user_spawn()                             (creates a kthread)
 *       |
 *       v  (when the scheduler picks the thread)
 *   user_thread_main()
 *       |
 *       v
 *   aarch64_enter_userspace(entry, sp_el0)
 *       msr SP_EL0, ELR_EL1, SPSR_EL1
 *       eret -> EL0 at user code
 *
 *   user code (running in EL0):
 *       mov x0, #msg_addr      ; pointer into user page
 *       mov x8, #SYS_log
 *       svc #0                  ; trap to EL1
 *   kernel:
 *       vectors.S VEC_SYNC_LO64 entry (offset 0x400)
 *       trap_dispatch, ESR.EC=0x15  (SVC from AArch64 lower EL)
 *       syscall_dispatch -> sys_log
 *       kprintf reads the user pointer (same TTBR0)
 *       eret -> back to EL0 next instruction (b .)
 *   user code: spins forever in EL0
 *
 * Backing storage and address spaces
 * ----------------------------------
 * We have one TTBR0 with kernel + user mappings co-resident; the
 * AP[2:1]=01 bit on the user 2 MB block is what makes it accessible
 * from EL0.  No TTBR1 split yet, no per-process page tables.
 *
 * user_storage is a 2 MB-aligned BSS region; mmu_map_user_2mb
 * remaps VA 0x10000000..0x10200000 to user_storage's PA with user
 * permissions, so the EL0 view at VA 0x10000000 == user_storage's
 * kernel-VA contents.
 *
 * Cache maintenance
 * -----------------
 * We just wrote bytes to user_storage as DATA.  The CPU's I-cache
 * has no entries for that PA yet, but it WILL fetch and cache once
 * EL0 starts executing.  To make sure the I-cache pulls our fresh
 * bytes (and not whatever happened to be in cache from before),
 * we do an IC IALLU + DSB ISH + ISB after the writes.  Heavy-handed
 * vs. per-VA IC IVAU but trivially correct for this scale.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/elf.h"
#include "kappara/kmem.h"
#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/string.h"
#include "kappara/syscall.h"
#include "kappara/user.h"
#include "kappara/vfs.h"

/* User VA where we'll publish the program.  Must be 2 MB aligned. */
#define USER_VA		0x10000000UL
#define USER_SIZE	0x00200000UL
#define USER_STACK_TOP	(USER_VA + USER_SIZE)

/*
 * Backing storage: a 2 MB-aligned slab in BSS.  This is the physical
 * memory the user 2 MB VA window will eventually map to.
 */
__attribute__((aligned(0x200000)))
static unsigned char user_storage[USER_SIZE];

/*
 * The user init binary, embedded into the kernel image by
 * arch/aarch64/userblob.S via .incbin "build/user/init.bin".  The
 * sources live in user/, the build rules in Makefile.  Bytes start
 * with _start (linker.ld in user/ forces .text._start first), so
 * entry is USER_VA + 0.
 */
extern char user_blob_start[];
extern char user_blob_end[];

extern void aarch64_enter_userspace(uint64_t entry, uint64_t sp_el0,
				    uint64_t arg);

void user_init(void)
{
	size_t blob_len = (size_t)(user_blob_end - user_blob_start);
	if (blob_len > USER_SIZE) {
		kprintf("user: init binary too big (%lu > %lu)\n",
			(unsigned long)blob_len, (unsigned long)USER_SIZE);
		return;
	}
	kmemcpy(user_storage, user_blob_start, blob_len);

	/*
	 * Cache maintenance.  user_storage was last touched as DATA;
	 * the CPU's I-cache has no entries for it yet, but if it ever
	 * speculatively prefetched stale lines they'd be invalid now.
	 * DC CVAU pushes the new bytes out of D-cache to unification;
	 * IC IALLU drops any I-cache lines that might've been brought
	 * in.  DSB ISH + ISB seal the order.
	 */
	__asm__ volatile (
		"dsb	ish\n"
		"ic	iallu\n"
		"dsb	ish\n"
		"isb\n"
		::: "memory");

	/* Publish to EL0 at USER_VA. */
	mmu_map_user_2mb(USER_VA, (uintptr_t)&user_storage[0]);

	kprintf("user: init binary loaded (%lu bytes) at PA=%p, "
		"EL0 entry VA=0x%lx\n",
		(unsigned long)blob_len,
		(void *)&user_storage[0],
		(unsigned long)USER_VA);
}

static void user_thread_main(void *arg)
{
	(void)arg;
	kprintf("user: kthread entering EL0 (entry=0x%lx, sp=0x%lx)\n",
		(unsigned long)USER_VA, (unsigned long)USER_STACK_TOP);
	aarch64_enter_userspace(USER_VA, USER_STACK_TOP, 0);
	/* unreachable */
}

void user_spawn(void)
{
	kthread_create("user-init", user_thread_main, NULL);
}

/*
 * Multi-process support.  Every spawned user thread shares the same
 * user address space (single 2 MB region at USER_VA), but each gets
 * its own user-mode stack carved out from the top of that region.
 * Stack 0 (USER_STACK_TOP) belongs to init; the spawn pool sits
 * below it at 64 KB intervals.
 *
 *     USER_VA + USER_SIZE  --+
 *                            |  64 KB  init stack
 *                            +--
 *                            |  64 KB  spawn[0] stack
 *                            +--
 *                            |  64 KB  spawn[1] stack
 *                            +-- ...
 *
 * With 64 KB stacks we get (USER_SIZE / 64 KB) - 1 = 31 spawn slots.
 * Plenty for a learning OS.
 */

/*
 * Exec VA constants are declared here (before sys_spawn_impl) so the
 * spawn function can validate exec-space entry points.  The storage
 * arrays and MMU mapping happen later in exec_space_init.
 */
#define EXEC_VA        0x20000000UL
#define EXEC_SIZE      0x00200000UL	/* 2 MB code region */
#define EXEC_STACK_VA  0x20200000UL
#define EXEC_STACK_TOP 0x20400000UL	/* SP starts here, grows down */

#define SPAWN_STACK_SIZE	0x10000UL
#define SPAWN_MAX		((USER_SIZE / SPAWN_STACK_SIZE) - 1)

static unsigned spawn_next;
static unsigned exec_spawn_next;

struct spawn_args {
	uint64_t entry;
	uint64_t sp;
	uint64_t arg;
};

static void spawn_thread_main(void *p)
{
	struct spawn_args a = *(struct spawn_args *)p;
	kfree(p);
	kprintf("user: spawn entering EL0 (entry=0x%lx, sp=0x%lx, arg=0x%lx)\n",
		(unsigned long)a.entry, (unsigned long)a.sp,
		(unsigned long)a.arg);
	aarch64_enter_userspace(a.entry, a.sp, a.arg);
	/* unreachable */
}

long sys_spawn_impl(uint64_t entry, uint64_t arg)
{
	uint64_t stack_top;

	if (entry >= USER_VA && entry < USER_VA + USER_SIZE) {
		if (spawn_next >= SPAWN_MAX) {
			kprintf("sys_spawn: init pool exhausted\n");
			return -1;
		}
		unsigned slot = ++spawn_next;
		stack_top = USER_VA + USER_SIZE - (uint64_t)slot * SPAWN_STACK_SIZE;
	} else if (entry >= EXEC_VA && entry < EXEC_VA + EXEC_SIZE) {
		if (exec_spawn_next >= SPAWN_MAX) {
			kprintf("sys_spawn: exec pool exhausted\n");
			return -1;
		}
		unsigned slot = ++exec_spawn_next;
		/* slot 1 = EXEC_STACK_TOP - 64KB; main exec thread owns the top 64KB */
		stack_top = EXEC_STACK_TOP - (uint64_t)slot * SPAWN_STACK_SIZE;
	} else {
		kprintf("sys_spawn: entry 0x%lx not in user range\n",
			(unsigned long)entry);
		return -1;
	}

	struct spawn_args *a = kmalloc(sizeof(*a));
	if (!a) return -1;
	a->entry = entry;
	a->sp    = stack_top;
	a->arg   = arg;

	struct kthread *t = kthread_create("spawn", spawn_thread_main, a);
	if (!t) { kfree(a); return -1; }

	/* Hand the child a copy of the parent's fd table so it inherits
	 * pipes, the console, and anything else the parent had open --
	 * the fork()-ish piece of spawn that makes pipework actually
	 * compose. */
	kthread_inherit_fds(t, curthread);
	return (long)t->tid;
}

void sys_exit_impl(void) __attribute__((noreturn));
void sys_exit_impl(void)
{
	kthread_exit();
	/* kthread_exit doesn't return, but the attribute helps the
	 * caller's control-flow analysis. */
	for (;;)
		;
}

/* =========================================================================
 * Exec address space and ELF loader
 * =========================================================================
 *
 * A second pair of 2 MB windows lets exec'd programs live separately from
 * init without per-process page tables.  Layout:
 *
 *   0x20000000 .. 0x20200000   EXEC_VA       code + data (exec_storage)
 *   0x20200000 .. 0x20400000   EXEC_STACK_VA stack backing (exec_stack_storage)
 *
 * Both windows are mapped once at exec_space_init time and reused for every
 * exec call (only one exec'd process at a time; the shell waits with
 * sys_wait before accepting the next command).
 *
 * Blob file ops
 * -------------
 * /bin programs are NOT stored on the kfs ramdisk -- they're ELF files
 * incbin'd into the kernel image itself (arch/aarch64/helloblob.S) and
 * registered in the VFS as read-only blob inodes.  No ramdisk
 * block-size constraints, no bitmap management.
 *
 * Note: EXEC_VA, EXEC_SIZE, EXEC_STACK_VA, EXEC_STACK_TOP are defined
 * earlier (near the spawn section) so sys_spawn_impl can check them.
 */

__attribute__((aligned(0x200000)))
static unsigned char exec_storage[EXEC_SIZE];

__attribute__((aligned(0x200000)))
static unsigned char exec_stack_storage[EXEC_SIZE];

/* ---- blob file_ops ---- */

struct blob_priv {
	const unsigned char *data;
	size_t               size;
};

struct blob_cursor {
	struct blob_priv *bp;
	size_t            pos;
};

static int blob_open(struct file *f)
{
	struct blob_cursor *c = kmalloc(sizeof(*c));
	if (!c) return -1;
	c->bp  = f->f_inode->i_private;
	c->pos = 0;
	f->f_private = c;
	return 0;
}

static int blob_close(struct file *f)
{
	kfree(f->f_private);
	f->f_private = NULL;
	return 0;
}

static long blob_read(struct file *f, void *buf, size_t len)
{
	struct blob_cursor *c = f->f_private;
	size_t avail = c->bp->size - c->pos;
	if (len > avail) len = avail;
	kmemcpy(buf, c->bp->data + c->pos, len);
	c->pos += len;
	return (long)len;
}

static long blob_seek(struct file *f, long off, int whence)
{
	struct blob_cursor *c  = f->f_private;
	long               sz  = (long)c->bp->size;
	long               np;
	if      (whence == 0) np = off;
	else if (whence == 1) np = (long)c->pos + off;
	else                  np = sz + off;
	if (np < 0 || np > sz) return -1;
	c->pos = (size_t)np;
	return np;
}

static long blob_size(struct inode *ino)
{
	return (long)((struct blob_priv *)ino->i_private)->size;
}

static struct file_ops blob_fops = {
	.open  = blob_open,
	.close = blob_close,
	.read  = blob_read,
	.seek  = blob_seek,
	.size  = blob_size,
};

/* ---- /bin and /usr/bin population ---- */

extern char hello_blob_start[];
extern char hello_blob_end[];

extern char ps_blob_start[];
extern char ps_blob_end[];
extern char sigtest_blob_start[];
extern char sigtest_blob_end[];
extern char masktest_blob_start[];
extern char masktest_blob_end[];
extern char waittest_blob_start[];
extern char waittest_blob_end[];
extern char segvtest_blob_start[];
extern char segvtest_blob_end[];
extern char crash_blob_start[];
extern char crash_blob_end[];
extern char pipe_blob_start[];
extern char pipe_blob_end[];
extern char pipework_blob_start[];
extern char pipework_blob_end[];

static struct blob_priv hello_priv;

void exec_space_init(void)
{
	/* Map exec code and stack windows to EL0. */
	mmu_map_user_2mb(EXEC_VA,       (uintptr_t)exec_storage);
	mmu_map_user_2mb(EXEC_STACK_VA, (uintptr_t)exec_stack_storage);

	/* Create /bin and register embedded ELF blobs. */
	struct dentry *bin = vfs_mkdir(vfs_root(), "bin");
	hello_priv.data = (const unsigned char *)hello_blob_start;
	hello_priv.size = (size_t)(hello_blob_end - hello_blob_start);
	vfs_mknod_regfile(bin, "hello", &blob_fops, &hello_priv);

	kprintf("exec: code=0x%lx stack_top=0x%lx, /bin/hello %lu bytes\n",
		(unsigned long)EXEC_VA,
		(unsigned long)EXEC_STACK_TOP,
		(unsigned long)hello_priv.size);

	/* Create /usr/bin hierarchy and register /usr/bin programs. */
	struct dentry *usr    = vfs_mkdir(vfs_root(), "usr");
	struct dentry *usrbin = vfs_mkdir(usr, "bin");

	static struct blob_priv ps_priv, sigtest_priv, masktest_priv,
	                        waittest_priv, segvtest_priv, crash_priv,
	                        pipe_priv, pipework_priv;

#define REG(dir, name, priv, s, e) do { \
	(priv).data = (const unsigned char *)(s); \
	(priv).size = (size_t)((e) - (s)); \
	vfs_mknod_regfile((dir), (name), &blob_fops, &(priv)); \
} while (0)

	REG(usrbin, "ps",       ps_priv,       ps_blob_start,       ps_blob_end);
	REG(usrbin, "sigtest",  sigtest_priv,  sigtest_blob_start,  sigtest_blob_end);
	REG(usrbin, "masktest", masktest_priv, masktest_blob_start, masktest_blob_end);
	REG(usrbin, "waittest", waittest_priv, waittest_blob_start, waittest_blob_end);
	REG(usrbin, "segvtest", segvtest_priv, segvtest_blob_start, segvtest_blob_end);
	REG(usrbin, "crash",    crash_priv,    crash_blob_start,    crash_blob_end);
	REG(usrbin, "pipe",     pipe_priv,     pipe_blob_start,     pipe_blob_end);
	REG(usrbin, "pipework", pipework_priv, pipework_blob_start, pipework_blob_end);

#undef REG

	kprintf("exec: /usr/bin registered: ps(%lu) sigtest(%lu) masktest(%lu) "
		"waittest(%lu) segvtest(%lu) crash(%lu) pipe(%lu) pipework(%lu)\n",
		(unsigned long)ps_priv.size,
		(unsigned long)sigtest_priv.size,
		(unsigned long)masktest_priv.size,
		(unsigned long)waittest_priv.size,
		(unsigned long)segvtest_priv.size,
		(unsigned long)crash_priv.size,
		(unsigned long)pipe_priv.size,
		(unsigned long)pipework_priv.size);
}

/* ---- ELF loader ---- */

/*
 * Read an entire VFS regular file into a kernel buffer, going directly
 * through f_ops->read so the destination buffer can be anywhere in
 * kernel address space (skipping the copy_to_user path in sys_read_impl).
 */
static long read_file_kernel(const char *path, void *buf, size_t maxsz)
{
	struct dentry *d = vfs_lookup(path);
	if (!d) { kprintf("execve: ENOENT '%s'\n", path); return -1; }
	struct inode *ino = d->d_inode;
	if (!ino || ino->i_type != INODE_REG || !ino->i_fops) return -1;

	/* Manually open without touching the fd table. */
	struct file *f = kmalloc(sizeof(*f));
	if (!f) return -1;
	f->f_ops     = ino->i_fops;
	f->f_inode   = ino;
	f->f_private = NULL;
	f->f_refs    = 1;
	f->f_flags   = 0;
	vfs_iget(ino);
	if (f->f_ops->open && f->f_ops->open(f) < 0) {
		vfs_iput(ino); kfree(f); return -1;
	}

	size_t pos = 0;
	while (pos < maxsz) {
		long n = f->f_ops->read(f, (char *)buf + pos, maxsz - pos);
		if (n <= 0) break;
		pos += (size_t)n;
	}
	if (f->f_ops->close) f->f_ops->close(f);
	vfs_iput(ino);
	kfree(f);
	return (long)pos;
}

/* Per-exec thread launch arguments. */
struct exec_args {
	uint64_t entry;
	uint64_t sp;
};

static void exec_thread_main(void *p)
{
	struct exec_args a = *(struct exec_args *)p;
	kfree(p);
	kprintf("exec: EL0 entry=0x%lx sp=0x%lx\n",
		(unsigned long)a.entry, (unsigned long)a.sp);
	aarch64_enter_userspace(a.entry, a.sp, 0);
	/* unreachable */
}

/*
 * 256 KB static read buffer.  aarch64-linux-gnu-ld pads ELFs to 4 KB
 * (with -z max-page-size=4096) or 64 KB (default) before the first LOAD
 * segment; 256 KB leaves plenty of headroom for the padding plus code.
 * Static so it lives in BSS rather than on the kernel stack.
 */
static unsigned char elf_read_buf[256 * 1024];

long sys_execve_impl(const char *path)
{
	long elf_sz = read_file_kernel(path, elf_read_buf, sizeof(elf_read_buf));
	if (elf_sz < (long)sizeof(Elf64_Ehdr)) {
		kprintf("execve: read failed (%ld bytes)\n", elf_sz);
		return -1;
	}

	Elf64_Ehdr *eh = (Elf64_Ehdr *)elf_read_buf;
	if (eh->e_ident[EI_MAG0] != 0x7f ||
	    eh->e_ident[EI_MAG1] != 'E'  ||
	    eh->e_ident[EI_MAG2] != 'L'  ||
	    eh->e_ident[EI_MAG3] != 'F') {
		kprintf("execve: bad ELF magic\n"); return -1;
	}
	if (eh->e_ident[EI_CLASS] != 2 || /* ELF64 */
	    eh->e_ident[EI_DATA]  != 1 || /* LE    */
	    eh->e_machine != EM_AARCH64 ||
	    eh->e_type    != ET_EXEC) {
		kprintf("execve: not an AArch64 ET_EXEC ELF\n"); return -1;
	}
	if (eh->e_phoff == 0 || eh->e_phnum == 0) {
		kprintf("execve: no program headers\n"); return -1;
	}

	/* Reset the exec-space spawn counter for the new program. */
	exec_spawn_next = 0;

	/* Zero exec storage so BSS segments start clean. */
	kmemset(exec_storage,       0, EXEC_SIZE);
	kmemset(exec_stack_storage, 0, EXEC_SIZE);

	/* Copy PT_LOAD segments into exec_storage. */
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = (Elf64_Phdr *)(elf_read_buf +
				 eh->e_phoff + (uint64_t)i * eh->e_phentsize);
		if (ph->p_type != PT_LOAD || ph->p_filesz == 0) continue;

		if (ph->p_vaddr < EXEC_VA ||
		    ph->p_vaddr + ph->p_filesz > EXEC_VA + EXEC_SIZE) {
			kprintf("execve: LOAD segment 0x%lx+0x%lx "
				"outside exec window\n",
				(unsigned long)ph->p_vaddr,
				(unsigned long)ph->p_filesz);
			return -1;
		}
		size_t dst_off = (size_t)(ph->p_vaddr - EXEC_VA);
		kmemcpy(exec_storage + dst_off,
			elf_read_buf + ph->p_offset,
			(size_t)ph->p_filesz);
	}

	/* Flush D-cache and invalidate I-cache for the new code pages. */
	__asm__ volatile (
		"dsb  ish\n"
		"ic   iallu\n"
		"dsb  ish\n"
		"isb\n"
		::: "memory");

	struct exec_args *a = kmalloc(sizeof(*a));
	if (!a) return -1;
	a->entry = eh->e_entry;
	a->sp    = EXEC_STACK_TOP;

	struct kthread *t = kthread_create("exec", exec_thread_main, a);
	if (!t) { kfree(a); return -1; }
	/* Inherit fd table so the exec'd program has /dev/console on fd 1. */
	kthread_inherit_fds(t, curthread);
	return (long)t->tid;
}

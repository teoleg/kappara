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

#include "kappara/blkdev.h"
#include "kappara/elf.h"
#include "kappara/kfs.h"
#include "kappara/kmem.h"
#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "kappara/process.h"
#include "kappara/sched.h"
#include "kappara/string.h"
#include "kappara/syscall.h"
#include "kappara/user.h"
#include "kappara/vfs.h"

/* User VA where we'll publish the program.  Must be 2 MB aligned. */
#define USER_VA		0x10000000UL
#define USER_SIZE	0x00200000UL
#define USER_STACK_TOP	(USER_VA + USER_SIZE)

/* User-mode stack slot size.  Used by both the multi-shell pool
 * (user_spawn) and sys_spawn workers, all carved from user_storage
 * top-down.  See the layout diagram further down. */
#define SPAWN_STACK_SIZE	0x10000UL

/* R5: per-process exec backing.  See the bigger comment block down at
 * the storage-array declarations for what each slot owns.  These
 * counters + lookup helpers are hoisted up here so sys_spawn_impl
 * (defined earlier in the file) can read them. */
#define EXEC_SLOTS	2

static int      exec_slot_used[EXEC_SLOTS];
static uint64_t exec_heap_brk[EXEC_SLOTS];
static unsigned exec_spawn_next_per_slot[EXEC_SLOTS];

/* Forward decls -- definitions follow. */
static int cur_exec_slot(void);

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

#include "kappara/trap.h"
#include "kappara/tty.h"
#include "kappara/user.h"
#include "kappara/vfs.h"

/* Forward decl -- defined later, near sys_spawn_impl. */
static unsigned spawn_next;

/* Per-init-shell launch args.  Each entry of the multi-shell pool
 * spawned by user_spawn carries the same EL0 entry point but a
 * different stack slot + tty number (in user x0). */
struct init_args {
	uint64_t entry;
	uint64_t sp;
	uint64_t tty;
};

static void user_thread_main(void *arg)
{
	struct init_args a = *(struct init_args *)arg;
	kfree(arg);
	kprintf("user: kthread entering EL0 tty=%lu (entry=0x%lx, sp=0x%lx)\n",
		(unsigned long)a.tty, (unsigned long)a.entry,
		(unsigned long)a.sp);
	aarch64_enter_userspace(a.entry, a.sp, a.tty);
	/* unreachable */
}

void user_spawn(void)
{
	/* Spawn one shell per /dev/ttyN.  Shell N runs at the EL0 entry
	 * USER_VA with SP = USER_STACK_TOP - N*64KB and x0 = N, so
	 * init's _start can open /dev/ttyN as fds 0/1/2.  Only the
	 * currently-active tty drives output to UART; inactive shells
	 * paint into their cell buffer and surface on tty_switch.
	 *
	 * Bump spawn_next past the multi-shell slots so any future
	 * sys_spawn-ed workers don't collide with the shell stacks. */
	for (unsigned i = 0; i < NTTY; i++) {
		struct init_args *a = kmalloc(sizeof(*a));
		if (!a) {
			kprintf("user_spawn: kmalloc failed for tty=%u\n", i);
			continue;
		}
		a->entry = USER_VA;
		a->sp    = USER_VA + USER_SIZE - (uint64_t)i * SPAWN_STACK_SIZE;
		a->tty   = i;

		char name[16];	/* "user-init-N" fits */
		const char *src = "user-init-";
		char       *dst = name;
		while (*src) *dst++ = *src++;
		*dst++ = (char)('0' + i);
		*dst   = '\0';

		struct kthread *t = kthread_create(name, user_thread_main, a);
		if (!t) { kfree(a); continue; }
		kthread_setclass(t, SCLASS_TS);
	}
	/* spawn_next is incremented BEFORE use in sys_spawn_impl, so the
	 * first sys_spawn worker wants `++spawn_next` to equal NTTY. */
	spawn_next = NTTY - 1;
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
#define EXEC_HEAP_VA   0x20400000UL	/* heap: grows up from here */
#define EXEC_HEAP_SIZE 0x00200000UL	/* 2 MB ceiling */

/* SPAWN_STACK_SIZE is defined near the top of the file. */
#define SPAWN_MAX		((USER_SIZE / SPAWN_STACK_SIZE) - 1)

/* spawn_next forward-declared earlier so user_spawn can reset it.
 * exec_spawn_next_per_slot[] is defined further down with the
 * per-process exec backing pool. */

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
		int es = cur_exec_slot();
		if (exec_spawn_next_per_slot[es] >= SPAWN_MAX) {
			kprintf("sys_spawn: exec pool exhausted (slot %d)\n", es);
			return -1;
		}
		unsigned slot = ++exec_spawn_next_per_slot[es];
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
	/* sys_spawn lands an EL0 thread: TS class so its priority
	 * ages under CPU consumption.  cl_fork stamps t_pri and
	 * t_quantum_left to the TS defaults. */
	kthread_setclass(t, SCLASS_TS);
	return (long)t->tid;
}

void sys_exit_impl(int status) __attribute__((noreturn));
void sys_exit_impl(int status)
{
	kthread_exit(status);
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

/*
 * R5: per-process exec backing.  Each "exec process slot" has its own
 * physical EXEC_VA/EXEC_STACK_VA/EXEC_HEAP_VA backing so fork() can
 * give the child a copy of the parent's address space without
 * sharing pages.  Slot 0 is the boot-table default (every exec runs
 * there until a fork happens); fork allocates slot 1 for the child.
 *
 * The per-process vm_map's L2 maps EXEC_VA / EXEC_STACK_VA /
 * EXEC_HEAP_VA to slot N's physical pages; mmu_vmap_switch swaps
 * TTBR0 on context switch so each thread sees its own slot.
 *
 * Memory budget: 2 slots * 6 MB = 12 MB BSS.  Bump EXEC_SLOTS to 4
 * to allow concurrent forks across multiple shells; cost is 6 MB
 * per extra slot.
 */
/* EXEC_SLOTS, exec_slot_used[], exec_heap_brk[],
 * exec_spawn_next_per_slot[], and cur_exec_slot() are forward-declared
 * up by SPAWN_STACK_SIZE so sys_spawn_impl can read them.  The big
 * 6 MB-per-slot storage arrays live here. */

__attribute__((aligned(0x200000)))
static unsigned char exec_storage[EXEC_SLOTS][EXEC_SIZE];

__attribute__((aligned(0x200000)))
static unsigned char exec_stack_storage[EXEC_SLOTS][EXEC_SIZE];

__attribute__((aligned(0x200000)))
static unsigned char exec_heap_storage[EXEC_SLOTS][EXEC_HEAP_SIZE];

static int exec_slot_alloc(void)
{
	for (int i = 0; i < EXEC_SLOTS; i++) {
		if (!exec_slot_used[i]) {
			exec_slot_used[i] = 1;
			return i;
		}
	}
	return -1;
}

static void exec_slot_free(int slot)
{
	if (slot >= 0 && slot < EXEC_SLOTS)
		exec_slot_used[slot] = 0;
}

void exec_slot_release(int slot)
{
	exec_slot_free(slot);
}

/* Resolve the exec slot for the current thread.  Returns 0 (the
 * boot slot) when the caller is on the boot vm_map -- works for
 * legacy code paths that haven't migrated to per-process slots. */
static int cur_exec_slot(void)
{
	struct kthread *t = curthread;
	if (!t || !t->t_proc || !t->t_proc->vm) return 0;
	int s = t->t_proc->vm->exec_slot;
	return (s >= 0) ? s : 0;
}

long sys_brk_impl(uint64_t addr)
{
	int slot = cur_exec_slot();
	if (addr == 0)
		return (long)exec_heap_brk[slot];
	if (addr < EXEC_HEAP_VA || addr > EXEC_HEAP_VA + EXEC_HEAP_SIZE)
		return -1;
	exec_heap_brk[slot] = addr;
	return (long)exec_heap_brk[slot];
}

/* ---- R5: fork ----------------------------------------------------- */

extern void *arch_thread_init_fork_frame(void *stack_top,
					 const struct trap_frame *parent_tf,
					 uint64_t child_sp_el0);

/* fork_child_main is the C-level thunk dispatched by kthread_create
 * for the new child.  We never actually want C code to run for the
 * child -- the fork_trampoline asm path handles eret directly via
 * the dual-frame layout.  But kthread_create's contract is that
 * the first scheduled run pops a swtch frame and ret's to
 * thread_trampoline, which calls fn(arg).  We bypass that by
 * pointing the swtch frame's x30 directly at fork_trampoline; this
 * thunk exists only as a defensive stub in case anyone follows the
 * normal path. */
static void fork_child_thunk(void *arg)
{
	(void)arg;
	kprintf("fork: child reached C fallback -- bug\n");
	kthread_exit(-1);
}

long sys_fork_impl(struct trap_frame *parent_tf)
{
	struct kthread *parent_t = curthread;
	if (!parent_t || !parent_t->t_proc || !parent_t->t_proc->vm) {
		kprintf("fork: no parent process\n");
		return -1;
	}
	int parent_slot = parent_t->t_proc->vm->exec_slot;
	if (parent_slot < 0) {
		kprintf("fork: parent is not exec'd (slot=%d)\n", parent_slot);
		return -1;
	}

	/* Refuse if parent's EL0 PC isn't in the exec window -- that
	 * means we're somehow being called from a non-exec'd context
	 * (an init shell, etc.) and the assumption that EXEC_VA covers
	 * the user code wouldn't hold. */
	if (parent_tf->elr < EXEC_VA || parent_tf->elr >= EXEC_VA + EXEC_SIZE) {
		kprintf("fork: parent elr 0x%lx outside exec window\n",
			(unsigned long)parent_tf->elr);
		return -1;
	}

	int child_slot = exec_slot_alloc();
	if (child_slot < 0) {
		kprintf("fork: no free exec slot (EXEC_SLOTS=%d)\n", EXEC_SLOTS);
		return -1;
	}

	/* Copy parent's slot contents into child's slot.  This is the
	 * eager-copy version of fork; COW is a future optimisation
	 * (would need 4 KB page mappings instead of the 2 MB blocks
	 * we use today). */
	kmemcpy(exec_storage[child_slot],       exec_storage[parent_slot],
	        EXEC_SIZE);
	kmemcpy(exec_stack_storage[child_slot], exec_stack_storage[parent_slot],
	        EXEC_SIZE);
	kmemcpy(exec_heap_storage[child_slot],  exec_heap_storage[parent_slot],
	        EXEC_HEAP_SIZE);

	/* Inherit heap break + spawn-pool counter. */
	exec_heap_brk[child_slot]            = exec_heap_brk[parent_slot];
	exec_spawn_next_per_slot[child_slot] = exec_spawn_next_per_slot[parent_slot];

	/* Flush D-cache + invalidate I-cache for the freshly-copied
	 * code pages so the child fetches them cleanly. */
	__asm__ volatile (
		"dsb  ish\n"
		"ic   iallu\n"
		"dsb  ish\n"
		"isb\n"
		::: "memory");

	/* Build child vm_map.  Same EL0 VAs, different PA backings. */
	struct vm_map *cvm = vm_map_create();
	if (!cvm) {
		kprintf("fork: vm_map_create failed\n");
		exec_slot_free(child_slot);
		return -1;
	}
	cvm->exec_slot = child_slot;
	mmu_vmap_map_user_2mb(cvm, EXEC_VA,
		(uintptr_t)exec_storage[child_slot]);
	mmu_vmap_map_user_2mb(cvm, EXEC_STACK_VA,
		(uintptr_t)exec_stack_storage[child_slot]);
	mmu_vmap_map_user_2mb(cvm, EXEC_HEAP_VA,
		(uintptr_t)exec_heap_storage[child_slot]);

	struct process *cp = process_alloc(cvm);
	vm_map_put(cvm);	/* process_alloc bumped the ref */
	if (!cp) {
		kprintf("fork: process_alloc failed\n");
		/* vm_map_put released the slot via exec_slot_release. */
		return -1;
	}

	/* Create the child kthread.  We deliberately use the
	 * "no_dispatch" variant + a dummy fn so we can stomp the saved
	 * kernel-SP with the fork_trampoline frame layout BEFORE the
	 * scheduler picks it up. */
	struct kthread *ct = kthread_create_no_dispatch(parent_t->name,
		fork_child_thunk, NULL);
	if (!ct) {
		process_put(cp);
		return -1;
	}

	/* Replace the inherited init_process backref with the child
	 * process.  process_put on the inherited proc drops the ref
	 * kthread_create_internal bumped. */
	process_put(ct->t_proc);
	ct->t_proc = cp;

	/* Inherit fds from the parent thread (same SVR4 fork shape). */
	kthread_inherit_fds(ct, parent_t);
	kthread_setclass(ct, SCLASS_TS);

	/* Rebuild the saved kernel-stack frame so the first scheduled
	 * run lands in fork_trampoline with a trap_frame copy laid out
	 * for KERNEL_EXIT.  Parent and child share the EL0 VA, so the
	 * child's sp_el0 is the parent's verbatim -- different physical
	 * page underneath. */
	ct->sp = arch_thread_init_fork_frame((char *)ct->stack_base + 4096,
					     parent_tf,
					     parent_tf->sp_el0);

	long child_tid = (long)ct->tid;
	kthread_dispatch(ct);
	return child_tid;
}

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
extern char malloctest_blob_start[];
extern char malloctest_blob_end[];
extern char forktest_blob_start[];
extern char forktest_blob_end[];

static struct blob_priv hello_priv;

void exec_space_init(void)
{
	/* Map slot 0 in the boot l0 table so that any code path that
	 * stumbles into EXEC_VA before an actual sys_execve still
	 * resolves to *some* valid backing.  We deliberately do NOT
	 * mark slot 0 as in-use here: exec'd processes each install
	 * their own vm_map, and the slot allocator should hand out
	 * slot 0 to whichever fork/exec asks first. */
	mmu_map_user_2mb(EXEC_VA,       (uintptr_t)exec_storage[0]);
	mmu_map_user_2mb(EXEC_STACK_VA, (uintptr_t)exec_stack_storage[0]);
	mmu_map_user_2mb(EXEC_HEAP_VA,  (uintptr_t)exec_heap_storage[0]);

	/* Create /bin and register embedded ELF blobs. */
	struct dentry *bin = vfs_mkdir(vfs_root(), "bin");
	hello_priv.data = (const unsigned char *)hello_blob_start;
	hello_priv.size = (size_t)(hello_blob_end - hello_blob_start);
	vfs_mknod_regfile(bin, "hello", &blob_fops, &hello_priv);

	kprintf("exec: code=0x%lx stack_top=0x%lx, /bin/hello %lu bytes\n",
		(unsigned long)EXEC_VA,
		(unsigned long)EXEC_STACK_TOP,
		(unsigned long)hello_priv.size);

	/* /etc: small static text files registered as in-memory blobs.
	 * Pre-R0 these lived on the ramdisk; the R0 move put the
	 * ramdisk under /usr/bin and orphaned them.  Re-registering as
	 * blob_fops gets them back without burning a second ramdisk. */
	{
		static const char readme_buf[] =
			"kappara: an SVR4-flavoured hobby kernel for AArch64.\n"
			"  /bin       in-kernel blob ELFs\n"
			"  /usr/bin   user programs on the kfs ramdisk\n"
			"  /etc       this directory (motd, readme)\n"
			"  /proc      kernel state via STREAMS cdevs\n"
			"  /dev       tty0..3, console, klog, fbcon, null, loop\n";
		static const char motd_buf[] =
			"no soup for you, only streams.\n";
		static struct blob_priv readme_priv = {
			.data = (const unsigned char *)readme_buf,
			.size = sizeof(readme_buf) - 1,
		};
		static struct blob_priv motd_priv = {
			.data = (const unsigned char *)motd_buf,
			.size = sizeof(motd_buf) - 1,
		};
		struct dentry *etc = vfs_mkdir(vfs_root(), "etc");
		vfs_mknod_regfile(etc, "readme", &blob_fops, &readme_priv);
		vfs_mknod_regfile(etc, "motd",   &blob_fops, &motd_priv);
	}

	/*
	 * /usr/bin -- R0: kfs-on-ramdisk instead of in-kernel blob
	 * registration.  Build a payload table from the cmd ELF blobs
	 * (still .incbin'd into the kernel image) and copy them into the
	 * ramdisk via mkimage; mount the resulting filesystem at
	 * /usr/bin.  The exec loader's read_file_kernel path is unchanged
	 * -- it just reads through the regfile file_ops, which now go
	 * through the buf cache and block device instead of memcpy from
	 * rodata.  Same code, real storage layer.
	 *
	 * The cmd blobs stay in kernel rodata for now because the
	 * raspi3b -kernel boot path has no initrd story; once the SD/EMMC
	 * driver (S2) lands, the same kfs path picks up bytes from real
	 * storage and the blobs can leave the kernel image.
	 */
#define PAY(name_str, sym_start, sym_end) \
	{ name_str, (sym_start), (uint32_t)((sym_end) - (sym_start)) }
	const struct kfs_payload usrbin_payloads[] = {
		PAY("ps",         ps_blob_start,         ps_blob_end),
		PAY("sigtest",    sigtest_blob_start,    sigtest_blob_end),
		PAY("masktest",   masktest_blob_start,   masktest_blob_end),
		PAY("waittest",   waittest_blob_start,   waittest_blob_end),
		PAY("segvtest",   segvtest_blob_start,   segvtest_blob_end),
		PAY("crash",      crash_blob_start,      crash_blob_end),
		PAY("pipe",       pipe_blob_start,       pipe_blob_end),
		PAY("pipework",   pipework_blob_start,   pipework_blob_end),
		PAY("malloctest", malloctest_blob_start, malloctest_blob_end),
		PAY("forktest",   forktest_blob_start,   forktest_blob_end),
	};
#undef PAY
	const unsigned n_usrbin = sizeof(usrbin_payloads) / sizeof(usrbin_payloads[0]);

	kfs_mkimage(ramdisk_get(), usrbin_payloads, n_usrbin);

	struct dentry *usr    = vfs_mkdir(vfs_root(), "usr");
	struct dentry *usrbin = vfs_mkdir(usr, "bin");
	if (kfs_mount(ramdisk_get(), usrbin) < 0)
		kprintf("exec: kfs_mount /usr/bin failed\n");
	else
		kprintf("exec: /usr/bin mounted from ramdisk (%u files)\n",
		        n_usrbin);
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

#define EXEC_MAX_ARGS    32
#define EXEC_MAX_ARGLEN  128

long sys_execve_impl(const char *path, int argc, const char *const argv[])
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

	/* R5: allocate a process slot.  Slot 0 is always available
	 * until a fork happens; with EXEC_SLOTS=2, the second exec on a
	 * busy system can fail.  Tune EXEC_SLOTS up for concurrent
	 * forks across shells. */
	int slot = exec_slot_alloc();
	if (slot < 0) {
		kprintf("execve: no free exec slot (EXEC_SLOTS=%d in use)\n",
			EXEC_SLOTS);
		return -1;
	}

	/* Reset this slot's spawn counter + heap break. */
	exec_spawn_next_per_slot[slot] = 0;
	exec_heap_brk[slot]            = EXEC_HEAP_VA;

	/* Zero this slot's exec storage so BSS segments start clean. */
	kmemset(exec_storage[slot],       0, EXEC_SIZE);
	kmemset(exec_stack_storage[slot], 0, EXEC_SIZE);
	kmemset(exec_heap_storage[slot],  0, EXEC_HEAP_SIZE);

	/* Copy PT_LOAD segments into this slot's exec_storage. */
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
			exec_slot_free(slot);
			return -1;
		}
		size_t dst_off = (size_t)(ph->p_vaddr - EXEC_VA);
		kmemcpy(exec_storage[slot] + dst_off,
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

	/* --- exec stack setup ---
	 *
	 * Layout (grows down from EXEC_STACK_TOP = 0x20400000):
	 *   [string data -- argv strings packed from top down]
	 *   [16-byte alignment pad]
	 *   [argv[argc] = NULL,  8 bytes]
	 *   [argv[argc-1],       8 bytes]
	 *   ...
	 *   [argv[0],            8 bytes]
	 *   [argc as uint64_t,   8 bytes]  <-- SP points here
	 */
	int effective_argc = (argc > EXEC_MAX_ARGS) ? EXEC_MAX_ARGS : argc;
	uint64_t sp = EXEC_STACK_TOP;
	uint64_t uva_strings[EXEC_MAX_ARGS];

	unsigned char *slot_stack = exec_stack_storage[slot];

	/* Pack strings from top down */
	for (int i = effective_argc - 1; i >= 0; i--) {
		size_t len = kstrlen(argv[i]) + 1;  /* include NUL */
		sp -= (uint64_t)len;
		sp &= ~(uint64_t)7;                 /* 8-byte align */
		size_t off = (size_t)(sp - EXEC_STACK_VA);
		kmemcpy(slot_stack + off, argv[i], len);
		uva_strings[i] = sp;
	}

	/* 16-byte align before pointer array */
	sp &= ~(uint64_t)15;

	/* The pointer table is (effective_argc + 1) words (pointers + NULL)
	 * plus 1 word for argc = effective_argc + 2 words total.
	 * If that count is odd the final SP lands on an 8-byte boundary,
	 * violating the AArch64 ABI 16-byte alignment requirement.
	 * Insert a padding word so the total is always even. */
	if ((effective_argc + 2) & 1) {
		sp -= 8;
		*(uint64_t *)(slot_stack + (sp - EXEC_STACK_VA)) = 0;
	}

	/* argv[argc] = NULL terminator */
	sp -= 8;
	*(uint64_t *)(slot_stack + (sp - EXEC_STACK_VA)) = 0;

	/* argv[argc-1] .. argv[0] */
	for (int i = effective_argc - 1; i >= 0; i--) {
		sp -= 8;
		*(uint64_t *)(slot_stack + (sp - EXEC_STACK_VA)) = uva_strings[i];
	}

	/* argc — SP is 16-byte aligned here */
	sp -= 8;
	*(uint64_t *)(slot_stack + (sp - EXEC_STACK_VA)) = (uint64_t)effective_argc;

	struct exec_args *a = kmalloc(sizeof(*a));
	if (!a) { exec_slot_free(slot); return -1; }
	a->entry = eh->e_entry;
	a->sp    = sp;   /* SP points at argc word on the exec stack */

	/* Compute the basename FIRST and pass it into kthread_create so
	 * t->name reflects the program name before the new thread is
	 * dispatched.  Otherwise, on SMP a peer CPU can pick up the
	 * thread and call /proc/ps's pb_str(t->name) while we're still
	 * setting t->comm here -- the snapshot would show the placeholder
	 * "exec" instead of e.g. "ps". */
	const char *base = path;
	for (const char *p = path; *p; p++)
		if (*p == '/') base = p + 1;

	struct kthread *t = kthread_create_no_dispatch(base,
	                                               exec_thread_main, a);
	if (!t) { kfree(a); return -1; }

	/* R5: each exec gets its own process + vm_map mapping the EL0
	 * windows to the freshly-allocated slot.  vm->exec_slot owns
	 * the slot release on the last vm_map_put.
	 *
	 * Order matters here: we MUST install the new t_proc on the
	 * thread BEFORE kthread_dispatch hands it to the scheduler.
	 * Otherwise the new thread could run with t_proc still
	 * pointing at init_process and end up using the wrong TTBR0
	 * the first time the dispatcher schedules it. */
	struct vm_map *vm = vm_map_create();
	if (!vm) {
		kprintf("execve: vm_map_create failed\n");
		kfree(a);
		exec_slot_free(slot);
		return -1;
	}
	vm->exec_slot = slot;
	mmu_vmap_map_user_2mb(vm, EXEC_VA,       (uintptr_t)exec_storage[slot]);
	mmu_vmap_map_user_2mb(vm, EXEC_STACK_VA, (uintptr_t)exec_stack_storage[slot]);
	mmu_vmap_map_user_2mb(vm, EXEC_HEAP_VA,  (uintptr_t)exec_heap_storage[slot]);

	struct process *p = process_alloc(vm);
	vm_map_put(vm);	/* process_alloc bumped vm refs */
	if (!p) {
		kprintf("execve: process_alloc failed\n");
		kfree(a);
		/* vm_map_put above already released the slot via
		 * exec_slot_release. */
		return -1;
	}

	/* Swap the inherited init_process backref for the fresh one.
	 * process_put on the inherited proc drops the ref kthread_create
	 * bumped; the new proc came in with refs=1 from process_alloc. */
	process_put(t->t_proc);
	t->t_proc = p;

	/* Inherit fd table so the exec'd program has /dev/console on fd 1. */
	kthread_inherit_fds(t, curthread);
	/* exec lands in EL0: TS class for priority aging. */
	kthread_setclass(t, SCLASS_TS);

	kthread_dispatch(t);
	return (long)t->tid;
}

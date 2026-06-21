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

#include "kappara/fs/blkdev.h"
#include "kappara/abi/elf.h"
#include "kappara/fs/kfs.h"
#include "kappara/core/kmem.h"
#include "kappara/arch/mmu.h"
#include "kappara/core/pmm.h"
#include "kappara/core/printk.h"
#include "kappara/proc/process.h"
#include "kappara/proc/sched.h"
#include "kappara/core/string.h"
#include "kappara/abi/syscall.h"
#include "kappara/proc/user.h"
#include "kappara/fs/vfs.h"

/* User VA where we'll publish the program.  Must be 2 MB aligned. */
#define USER_VA		0x10000000UL
#define USER_SIZE	0x00200000UL
#define USER_STACK_TOP	(USER_VA + USER_SIZE)

/* User-mode stack slot size.  Used by both the multi-shell pool
 * (user_spawn) and sys_spawn workers, all carved from user_storage
 * top-down.  See the layout diagram further down. */
#define SPAWN_STACK_SIZE	0x10000UL

/* R6: per-process exec state lives on the current thread's vm_map.
 * Helper returns a pointer at the vm_map's spawn-slot counter, or
 * NULL if the caller isn't on a per-process vm_map (init shell on
 * the boot map). */
static unsigned *cur_exec_spawn_next(void)
{
	struct kthread *t = curthread;
	if (!t || !t->t_proc || !t->t_proc->vm) return 0;
	struct vm_map *vm = t->t_proc->vm;
	if (vm->heap_brk == 0) return 0;	/* boot vm_map, no per-proc state */
	return &vm->spawn_next;
}

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

#include "kappara/arch/trap.h"
#include "kappara/io/tty.h"
#include "kappara/proc/user.h"
#include "kappara/fs/vfs.h"

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
		unsigned *sn = cur_exec_spawn_next();
		if (!sn) {
			kprintf("sys_spawn: no per-process spawn counter\n");
			return -1;
		}
		if (*sn >= SPAWN_MAX) {
			kprintf("sys_spawn: exec pool exhausted\n");
			return -1;
		}
		unsigned slot = ++*sn;
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
 * R6: per-process exec memory backing.
 *
 * Each exec'd process owns its own vm_map (L0/L1/L2/L3 page tables)
 * with EXEC_VA/EXEC_STACK_VA/EXEC_HEAP_VA mapped 4 KB at a time onto
 * PMM-allocated pages.  No fixed slot pool; the number of concurrent
 * processes is bounded only by PMM size.
 *
 * Page lifecycle:
 *   - sys_execve allocates code pages (one per 4 KB of PT_LOAD), the
 *     full stack region (512 pages = 2 MB), and zero heap pages.
 *     The heap break starts at EXEC_HEAP_VA and grows via sys_brk.
 *   - sys_fork allocates fresh pages for every user page in the
 *     parent's vm_map and kmemcpys content; child's heap_brk and
 *     spawn_next inherit from parent.
 *   - On process exit, vm_map_put -> mmu_vmap_destroy walks L3
 *     tables, pmm_free's user pages, then frees the L3s + L0/L1/L2.
 */

/* Allocate `count` fresh PMM pages and map them at consecutive 4 KB
 * VAs starting at va_base in vm.  Returns 0 on success, -1 on PMM
 * exhaustion (already-mapped pages stay mapped for the caller to
 * clean up via mmu_vmap_destroy). */
static int vmap_alloc_pages(struct vm_map *vm, uint64_t va_base,
                            unsigned count, int zero)
{
	for (unsigned i = 0; i < count; i++) {
		void *p = pmm_alloc();
		if (!p) return -1;
		if (zero) kmemset(p, 0, PAGE_SIZE);
		if (mmu_vmap_map_user_4k(vm, va_base + (uint64_t)i * PAGE_SIZE,
		                         (uintptr_t)p) < 0) {
			pmm_free(p);
			return -1;
		}
	}
	return 0;
}

/* Copy bytes from the kernel into a user VA inside vm.  Walks vm's
 * L3 for each page to find its kernel-identity address.  Caller is
 * responsible for making sure the user VA range is mapped. */
static int vmap_copyin(struct vm_map *vm, uint64_t user_va,
                       const void *src, size_t len)
{
	const unsigned char *s = src;
	while (len > 0) {
		void *kva = mmu_vmap_user_va_to_kva(vm, user_va);
		if (!kva) return -1;
		size_t avail = PAGE_SIZE - (user_va & PAGE_MASK);
		size_t n     = (len < avail) ? len : avail;
		kmemcpy(kva, s, n);
		s        += n;
		user_va  += n;
		len      -= n;
	}
	return 0;
}

long sys_brk_impl(uint64_t addr)
{
	struct kthread *t = curthread;
	if (!t || !t->t_proc || !t->t_proc->vm) return -1;
	struct vm_map *vm = t->t_proc->vm;
	if (vm->heap_brk == 0) return -1;	/* boot vm_map */

	if (addr == 0) return (long)vm->heap_brk;
	if (addr < EXEC_HEAP_VA || addr > EXEC_HEAP_VA + EXEC_HEAP_SIZE)
		return -1;

	uint64_t old_brk = vm->heap_brk;
	uint64_t old_top = (old_brk + PAGE_SIZE - 1) & ~(uint64_t)PAGE_MASK;
	uint64_t new_top = (addr     + PAGE_SIZE - 1) & ~(uint64_t)PAGE_MASK;

	if (new_top > old_top) {
		/* Grow: allocate + map (new_top - old_top) / PAGE_SIZE pages. */
		unsigned n = (unsigned)((new_top - old_top) / PAGE_SIZE);
		if (vmap_alloc_pages(vm, old_top, n, /*zero=*/1) < 0) {
			kprintf("sys_brk: PMM exhausted growing heap to 0x%lx\n",
				(unsigned long)addr);
			return -1;
		}
	} else if (new_top < old_top) {
		/* Shrink: unmap + free pages [new_top..old_top). */
		for (uint64_t v = new_top; v < old_top; v += PAGE_SIZE)
			mmu_vmap_unmap_user_4k(vm, v);
	}

	vm->heap_brk = addr;
	/* Heap shape changed; flush TLB so subsequent loads see fresh
	 * mappings.  mmu_vmap_switch issues a tlbi vmalle1; we mirror
	 * that here for the simpler "current process changed its own
	 * map" case. */
	__asm__ volatile (
		"dsb	ishst\n"
		"tlbi	vmalle1\n"
		"dsb	ish\n"
		"isb\n" ::: "memory");
	return (long)vm->heap_brk;
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

/* Walk each user VA in the parent's vm_map, allocate a fresh PMM
 * page in the child, copy parent->child bytes, install the child
 * mapping.  Skips unmapped pages so we only touch what's live. */
static int fork_clone_pages(struct vm_map *cvm, struct vm_map *pvm,
                            uint64_t va_start, uint64_t va_end)
{
	for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
		void *src = mmu_vmap_user_va_to_kva(pvm, va);
		if (!src) continue;	/* not mapped in parent */
		void *p = pmm_alloc();
		if (!p) return -1;
		kmemcpy(p, src, PAGE_SIZE);
		if (mmu_vmap_map_user_4k(cvm, va, (uintptr_t)p) < 0) {
			pmm_free(p);
			return -1;
		}
	}
	return 0;
}

long sys_fork_impl(struct trap_frame *parent_tf)
{
	struct kthread *parent_t = curthread;
	if (!parent_t || !parent_t->t_proc || !parent_t->t_proc->vm) {
		kprintf("fork: no parent process\n");
		return -1;
	}
	struct vm_map *pvm = parent_t->t_proc->vm;
	if (pvm->heap_brk == 0) {
		kprintf("fork: parent is not exec'd (boot vm_map)\n");
		return -1;
	}

	/* Refuse if parent's EL0 PC isn't in the exec window. */
	if (parent_tf->elr < EXEC_VA || parent_tf->elr >= EXEC_VA + EXEC_SIZE) {
		kprintf("fork: parent elr 0x%lx outside exec window\n",
			(unsigned long)parent_tf->elr);
		return -1;
	}

	/* Build child vm_map and walk parent's three user regions
	 * (code, stack, heap), per-page allocating + copying. */
	struct vm_map *cvm = vm_map_create();
	if (!cvm) {
		kprintf("fork: vm_map_create failed\n");
		return -1;
	}

	if (fork_clone_pages(cvm, pvm, EXEC_VA, EXEC_VA + EXEC_SIZE) < 0 ||
	    fork_clone_pages(cvm, pvm, EXEC_STACK_VA,
	                     EXEC_STACK_VA + EXEC_SIZE) < 0 ||
	    fork_clone_pages(cvm, pvm, EXEC_HEAP_VA,
	                     EXEC_HEAP_VA + EXEC_HEAP_SIZE) < 0) {
		kprintf("fork: PMM exhausted during page clone\n");
		vm_map_put(cvm);	/* destroy frees the partial mappings */
		return -1;
	}

	cvm->heap_brk   = pvm->heap_brk;
	cvm->spawn_next = pvm->spawn_next;

	/* Flush D-cache + invalidate I-cache for the freshly-copied
	 * code pages so the child fetches them cleanly. */
	__asm__ volatile (
		"dsb  ish\n"
		"ic   iallu\n"
		"dsb  ish\n"
		"isb\n"
		::: "memory");

	struct process *cp = process_alloc(cvm);
	vm_map_put(cvm);	/* process_alloc bumped the ref */
	if (!cp) {
		kprintf("fork: process_alloc failed\n");
		/* vm_map_put on cvm above freed the user pages. */
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
extern char ping_blob_start[];
extern char ping_blob_end[];
extern char ifconfig_blob_start[];
extern char ifconfig_blob_end[];
extern char netstat_blob_start[];
extern char netstat_blob_end[];
extern char test_blob_start[];
extern char test_blob_end[];
extern char tcpconnect_blob_start[];
extern char tcpconnect_blob_end[];
extern char ftpd_blob_start[];
extern char ftpd_blob_end[];
extern char ls_blob_start[];
extern char ls_blob_end[];
extern char cat_blob_start[];
extern char cat_blob_end[];
extern char cp_blob_start[];
extern char cp_blob_end[];
extern char mv_blob_start[];
extern char mv_blob_end[];
extern char rm_blob_start[];
extern char rm_blob_end[];
extern char ll_blob_start[];
extern char ll_blob_end[];
extern char head_blob_start[];
extern char head_blob_end[];
extern char tail_blob_start[];
extern char tail_blob_end[];
extern char wc_blob_start[];
extern char wc_blob_end[];
extern char grep_blob_start[];
extern char grep_blob_end[];
extern char echo_blob_start[];
extern char echo_blob_end[];
extern char uptime_blob_start[];
extern char uptime_blob_end[];

static struct blob_priv hello_priv;

void exec_space_init(void)
{
	/* R6: no boot-table mapping needed for EXEC_VA -- each exec'd
	 * process installs its own per-page mappings via vm_map.
	 * Anything that stumbles into EXEC_VA without a per-process
	 * vm_map will translation-fault (caught by trap_dispatch). */

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
			"  /dev       tty0..3, lo0, icmp, console, klog, null\n";
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
		PAY("ping",       ping_blob_start,       ping_blob_end),
		PAY("ifconfig",   ifconfig_blob_start,   ifconfig_blob_end),
		PAY("netstat",    netstat_blob_start,    netstat_blob_end),
		PAY("test",       test_blob_start,       test_blob_end),
		PAY("tcpconnect", tcpconnect_blob_start, tcpconnect_blob_end),
		PAY("ftpd",       ftpd_blob_start,       ftpd_blob_end),
		PAY("ls",         ls_blob_start,         ls_blob_end),
		PAY("cat",        cat_blob_start,        cat_blob_end),
		PAY("cp",         cp_blob_start,         cp_blob_end),
		PAY("mv",         mv_blob_start,         mv_blob_end),
		PAY("rm",         rm_blob_start,         rm_blob_end),
		PAY("ll",         ll_blob_start,         ll_blob_end),
		PAY("head",       head_blob_start,       head_blob_end),
		PAY("tail",       tail_blob_start,       tail_blob_end),
		PAY("wc",         wc_blob_start,         wc_blob_end),
		PAY("grep",       grep_blob_start,       grep_blob_end),
		PAY("echo",       echo_blob_start,       echo_blob_end),
		PAY("uptime",     uptime_blob_start,     uptime_blob_end),
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

	/* Step 2 of the FTPD plan: empty kfs ramdisk mounted at /home.
	 * This is where future FTP STORs land -- separate from /usr/bin
	 * so a STOR can't overwrite a binary mid-exec.  Pass NULL/0 for
	 * an empty payload table; the on-disk root dirent stays empty. */
	kfs_mkimage(ramdisk_home_get(), NULL, 0);
	struct dentry *home = vfs_mkdir(vfs_root(), "home");
	if (kfs_mount(ramdisk_home_get(), home) < 0)
		kprintf("exec: kfs_mount /home failed\n");
	else
		kprintf("exec: /home mounted from ramdisk1 (writable)\n");
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
	    (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)) {
		kprintf("execve: not an AArch64 ET_EXEC/ET_DYN ELF\n"); return -1;
	}
	if (eh->e_phoff == 0 || eh->e_phnum == 0) {
		kprintf("execve: no program headers\n"); return -1;
	}

	/* DYNAMIC.md stage 3+4: ET_DYN binaries (PIE / static-pie) carry
	 * p_vaddr values relative to 0; we pick load_base = EXEC_VA at
	 * runtime and relocate.  ET_EXEC binaries (the hello-blob et al.)
	 * still carry absolute p_vaddr in the EXEC_VA window and need no
	 * fixup -- load_base = 0 leaves p_vaddr alone.
	 *
	 * One offset value drives both PT_LOAD placement and the
	 * R_AARCH64_RELATIVE walk; e_entry gets the same treatment so the
	 * trap frame points at the right instruction. */
	const uint64_t load_base = (eh->e_type == ET_DYN) ? EXEC_VA : 0;

	/* R6: build a fresh vm_map and allocate per-page user storage
	 * from PMM.  No fixed slot pool.  Order of operations:
	 *   1. vm_map_create
	 *   2. map + alloc code pages, copy PT_LOAD bytes into them
	 *   3. map + alloc the full 2 MB stack range
	 *   4. set heap_brk = EXEC_HEAP_VA (no heap pages yet)
	 *   5. write argv onto the now-mapped stack
	 *   6. process_alloc + bind to thread + dispatch
	 *
	 * Any failure mid-way unwinds via vm_map_put -> mmu_vmap_destroy,
	 * which walks the L3 tables and frees the partially-installed
	 * user pages. */
	struct vm_map *vm = vm_map_create();
	if (!vm) {
		kprintf("execve: vm_map_create failed\n");
		return -1;
	}

	/* (2) Code: walk PT_LOAD, allocate pages, copy bytes.
	 *
	 * For ET_DYN binaries this uses load_base = EXEC_VA so multiple
	 * PT_LOADs that the linker emitted at p_vaddr 0, 0x1000, 0x2000
	 * end up at EXEC_VA + offset.  For ET_EXEC, load_base = 0 so the
	 * (absolute) p_vaddr is used as-is. */
	uint64_t pt_dynamic_vaddr = 0;
	uint64_t pt_dynamic_filesz = 0;
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = (Elf64_Phdr *)(elf_read_buf +
				 eh->e_phoff + (uint64_t)i * eh->e_phentsize);
		if (ph->p_type == PT_DYNAMIC) {
			pt_dynamic_vaddr  = ph->p_vaddr + load_base;
			pt_dynamic_filesz = ph->p_filesz;
			continue;
		}
		if (ph->p_type != PT_LOAD) continue;

		uint64_t va = ph->p_vaddr + load_base;
		if (va < EXEC_VA ||
		    va + ph->p_memsz > EXEC_VA + EXEC_SIZE) {
			kprintf("execve: LOAD segment 0x%lx+0x%lx "
				"outside exec window\n",
				(unsigned long)va,
				(unsigned long)ph->p_memsz);
			vm_map_put(vm);
			return -1;
		}
		uint64_t seg_start = va & ~(uint64_t)PAGE_MASK;
		uint64_t seg_end   = (va + ph->p_memsz + PAGE_SIZE - 1)
		                     & ~(uint64_t)PAGE_MASK;
		unsigned n_pages   = (unsigned)((seg_end - seg_start) / PAGE_SIZE);
		if (vmap_alloc_pages(vm, seg_start, n_pages, /*zero=*/1) < 0) {
			kprintf("execve: PMM exhausted allocating code\n");
			vm_map_put(vm);
			return -1;
		}
		if (ph->p_filesz > 0) {
			if (vmap_copyin(vm, va,
			                elf_read_buf + ph->p_offset,
			                ph->p_filesz) < 0) {
				vm_map_put(vm);
				return -1;
			}
		}
	}

	/* (2b) Apply R_AARCH64_RELATIVE relocations from PT_DYNAMIC -> DT_RELA.
	 *
	 * Static-pie binaries have a .rela.dyn table of relative relocs.
	 * For each entry the loader writes (load_base + r_addend) at
	 * (load_base + r_offset).  GLOB_DAT and JUMP_SLOT show up only
	 * once libc.so is split out (stage 5+); for now we treat them
	 * as a hard error -- something compiled with -fPIC and DT_NEEDED.
	 *
	 * The walk is two-pass: first scan the Dyn array for DT_RELA /
	 * DT_RELASZ / DT_RELAENT, then apply each Rela.
	 */
	if (eh->e_type == ET_DYN && pt_dynamic_vaddr != 0) {
		uint64_t rela_off  = 0;
		uint64_t rela_size = 0;
		uint64_t rela_ent  = sizeof(Elf64_Rela);
		const Elf64_Dyn *dyn_arr = NULL;

		/* PT_DYNAMIC sits inside one of the PT_LOADs we just copied.
		 * Its file image is contiguous in elf_read_buf -- the simpler
		 * path is to walk it there rather than via vmap_copyin. */
		for (unsigned i = 0; i < eh->e_phnum; i++) {
			Elf64_Phdr *ph = (Elf64_Phdr *)(elf_read_buf +
					 eh->e_phoff + (uint64_t)i * eh->e_phentsize);
			if (ph->p_type != PT_DYNAMIC) continue;
			dyn_arr = (const Elf64_Dyn *)
			          (elf_read_buf + ph->p_offset);
			break;
		}
		if (!dyn_arr) {
			kprintf("execve: ET_DYN without PT_DYNAMIC payload\n");
			vm_map_put(vm);
			return -1;
		}
		unsigned dyn_n = (unsigned)(pt_dynamic_filesz / sizeof(Elf64_Dyn));
		for (unsigned i = 0; i < dyn_n; i++) {
			if (dyn_arr[i].d_tag == DT_NULL)    break;
			if (dyn_arr[i].d_tag == DT_RELA)    rela_off  = dyn_arr[i].d_un;
			if (dyn_arr[i].d_tag == DT_RELASZ)  rela_size = dyn_arr[i].d_un;
			if (dyn_arr[i].d_tag == DT_RELAENT) rela_ent  = dyn_arr[i].d_un;
			if (dyn_arr[i].d_tag == DT_NEEDED) {
				kprintf("execve: DT_NEEDED requires dynamic linker "
					"(stage 5)\n");
				vm_map_put(vm);
				return -1;
			}
		}

		if (rela_size > 0 && rela_ent == sizeof(Elf64_Rela)) {
			/* DT_RELA gives the load-relative *image* offset of the
			 * rela table.  Locate it in the file image via the file
			 * offset of whichever PT_LOAD covers it. */
			const Elf64_Rela *relas = NULL;
			for (unsigned i = 0; i < eh->e_phnum; i++) {
				Elf64_Phdr *ph = (Elf64_Phdr *)(elf_read_buf +
						 eh->e_phoff + (uint64_t)i * eh->e_phentsize);
				if (ph->p_type != PT_LOAD) continue;
				if (rela_off >= ph->p_vaddr &&
				    rela_off + rela_size <=
				        ph->p_vaddr + ph->p_filesz) {
					relas = (const Elf64_Rela *)
					        (elf_read_buf + ph->p_offset +
					         (rela_off - ph->p_vaddr));
					break;
				}
			}
			if (!relas) {
				kprintf("execve: DT_RELA outside PT_LOAD images\n");
				vm_map_put(vm);
				return -1;
			}
			unsigned n_rel = (unsigned)(rela_size / rela_ent);
			for (unsigned i = 0; i < n_rel; i++) {
				uint32_t t = ELF64_R_TYPE(relas[i].r_info);
				if (t == R_AARCH64_NONE) continue;
				if (t != R_AARCH64_RELATIVE) {
					kprintf("execve: reloc type %u not supported "
						"(stage 5+)\n", t);
					vm_map_put(vm);
					return -1;
				}
				uint64_t fixup = load_base + (uint64_t)relas[i].r_addend;
				uint64_t va    = load_base + relas[i].r_offset;
				if (vmap_copyin(vm, va, &fixup, sizeof(fixup)) < 0) {
					kprintf("execve: reloc target 0x%lx unmapped\n",
						(unsigned long)va);
					vm_map_put(vm);
					return -1;
				}
			}
		}
	}

	/* (3) Stack: allocate the entire 2 MB region upfront.  TODO:
	 * lazy/demand allocation via EL0 page faults is a future R6b. */
	if (vmap_alloc_pages(vm, EXEC_STACK_VA, EXEC_SIZE / PAGE_SIZE,
	                     /*zero=*/1) < 0) {
		kprintf("execve: PMM exhausted allocating stack\n");
		vm_map_put(vm);
		return -1;
	}

	/* Flush D-cache and invalidate I-cache for the new code pages. */
	__asm__ volatile (
		"dsb  ish\n"
		"ic   iallu\n"
		"dsb  ish\n"
		"isb\n"
		::: "memory");

	/* (4) Initialise heap break + per-process spawn counter. */
	vm->heap_brk   = EXEC_HEAP_VA;
	vm->spawn_next = 0;

	/* (5) exec stack setup -- write argv onto the mapped stack via
	 * vmap_copyin (which walks vm's L3 to find each page).
	 *
	 * Stack layout (grows down from EXEC_STACK_TOP):
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

	/* Pack strings from top down */
	for (int i = effective_argc - 1; i >= 0; i--) {
		size_t len = kstrlen(argv[i]) + 1;  /* include NUL */
		sp -= (uint64_t)len;
		sp &= ~(uint64_t)7;                 /* 8-byte align */
		if (vmap_copyin(vm, sp, argv[i], len) < 0) {
			kprintf("execve: argv string copyin failed\n");
			vm_map_put(vm);
			return -1;
		}
		uva_strings[i] = sp;
	}

	/* 16-byte align before pointer array */
	sp &= ~(uint64_t)15;

	if ((effective_argc + 2) & 1) {
		sp -= 8;
		uint64_t zero = 0;
		vmap_copyin(vm, sp, &zero, sizeof(zero));
	}

	/* argv[argc] = NULL terminator */
	sp -= 8;
	uint64_t null_word = 0;
	vmap_copyin(vm, sp, &null_word, sizeof(null_word));

	/* argv[argc-1] .. argv[0] */
	for (int i = effective_argc - 1; i >= 0; i--) {
		sp -= 8;
		vmap_copyin(vm, sp, &uva_strings[i], sizeof(uva_strings[i]));
	}

	/* argc — SP is 16-byte aligned here */
	sp -= 8;
	uint64_t argc_word = (uint64_t)effective_argc;
	vmap_copyin(vm, sp, &argc_word, sizeof(argc_word));

	struct exec_args *a = kmalloc(sizeof(*a));
	if (!a) { vm_map_put(vm); return -1; }
	a->entry = eh->e_entry + load_base;
	a->sp    = sp;   /* SP points at argc word on the exec stack */

	const char *base = path;
	for (const char *p = path; *p; p++)
		if (*p == '/') base = p + 1;

	struct kthread *t = kthread_create_no_dispatch(base,
	                                               exec_thread_main, a);
	if (!t) { kfree(a); vm_map_put(vm); return -1; }

	struct process *p = process_alloc(vm);
	vm_map_put(vm);	/* process_alloc bumped vm refs */
	if (!p) {
		kprintf("execve: process_alloc failed\n");
		kfree(a);
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

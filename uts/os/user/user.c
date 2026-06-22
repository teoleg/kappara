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
#include "kappara/core/uaccess.h"
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

/* DYNAMIC.md stage 5: ld-kappara.so window.  The stage 5 dynamic
 * linker is ET_EXEC at this fixed VA (deliberately not PIE -- it's
 * the bootstrap; relocating itself before it can run is a
 * chicken-and-egg problem).  The kernel loads it here alongside any
 * ET_DYN application and points the trap frame at LD_VA's _start. */
#define LD_VA          0x30000000UL
#define LD_SIZE        0x00100000UL	/* 1 MB ceiling for ld.so */

/* DYNAMIC.md stage 6: libc.so window.  Shared C library, ET_DYN
 * (PIE), loaded by the kernel at this fixed VA for every ET_DYN
 * exec.  Kernel applies its RELATIVE relocs and resolves the app's
 * GLOB_DAT / JUMP_SLOT entries against libc.so's dynsym.  Stage 7
 * (dlopen) moves the resolution into ld-kappara.so user-space.
 *
 * Sits at 0x38000000 (NOT 0x40000000) because each vm_map uses a
 * single L2 table backing the first 1 GB of VA (L1[0] -> L2);
 * L1[1..511] map peripheral and unused space.  Writes to 0x40000000
 * land on the peripheral block, not user RAM. */
#define LIBC_VA        0x38000000UL
#define LIBC_SIZE      0x00200000UL	/* 2 MB ceiling for libc.so */

/* DYNAMIC.md stage 7: runtime-loaded shared objects (dlopen).  Up to
 * DLOPEN_MAX_SLOTS concurrent objects in [DLOPEN_VA, DLOPEN_VA + 2 MB);
 * each slot owns a fixed 512 KB sub-window.  Inside the first 1 GB (same
 * reason as LIBC_VA). */
#define DLOPEN_VA        0x39000000UL
#define DLOPEN_SIZE      0x00200000UL	/* 2 MB total */
#define DLOPEN_SLOT_SIZE 0x00080000UL	/* 512 KB per slot, * 4 slots */

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

	/* Refuse if parent's EL0 PC isn't in one of the user code windows.
	 * With libc.so split out (DYNAMIC.md stage 6) the syscall site can
	 * sit in libc.so (LIBC_VA) -- e.g. test.c calling fork() jumps into
	 * libc.so's wrapper, ELR_EL1 there. */
	uint64_t pc = parent_tf->elr;
	int pc_in_exec = (pc >= EXEC_VA && pc <  EXEC_VA + EXEC_SIZE);
	int pc_in_libc = (pc >= LIBC_VA && pc <  LIBC_VA + LIBC_SIZE);
	if (!pc_in_exec && !pc_in_libc) {
		kprintf("fork: parent elr 0x%lx outside exec/libc windows\n",
			(unsigned long)pc);
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
	                     EXEC_HEAP_VA + EXEC_HEAP_SIZE) < 0 ||
	    /* DYNAMIC.md stages 5/6: child also needs ld.so + libc.so
	     * windows.  fork_clone_pages walks every mapped page in the
	     * range and skips holes, so unused tail pages cost nothing. */
	    fork_clone_pages(cvm, pvm, LD_VA, LD_VA + LD_SIZE) < 0 ||
	    fork_clone_pages(cvm, pvm, LIBC_VA, LIBC_VA + LIBC_SIZE) < 0 ||
	    /* DYNAMIC.md stage 7: clone the dlopen window too if anything's
	     * loaded.  fork_clone_pages skips unmapped pages so the cost
	     * is zero for processes that don't dlopen. */
	    fork_clone_pages(cvm, pvm, DLOPEN_VA, DLOPEN_VA + DLOPEN_SIZE) < 0) {
		kprintf("fork: PMM exhausted during page clone\n");
		vm_map_put(cvm);	/* destroy frees the partial mappings */
		return -1;
	}

	/* Inherit the dlopen slot table so the child can dlsym/dlclose the
	 * parent's handles.  Each slot's path is a fixed embedded buffer
	 * (kernel-stack copy of the syscall arg wouldn't survive past
	 * return, so we don't keep pointers). */
	for (unsigned s = 0; s < DLOPEN_MAX_SLOTS; s++) {
		cvm->dlopen_slots[s].base = pvm->dlopen_slots[s].base;
		unsigned i = 0;
		for (; i + 1 < sizeof(cvm->dlopen_slots[s].path) &&
		       pvm->dlopen_slots[s].path[i]; i++)
			cvm->dlopen_slots[s].path[i] = pvm->dlopen_slots[s].path[i];
		cvm->dlopen_slots[s].path[i] = '\0';
	}
	cvm->dlerror_msg[0] = '\0';

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
extern char nm_blob_start[];
extern char nm_blob_end[];
extern char ldd_blob_start[];
extern char ldd_blob_end[];
extern char objdump_blob_start[];
extern char objdump_blob_end[];

/* DYNAMIC.md stage 5: ld-kappara.so is referenced directly by the
 * exec loader; it doesn't go through VFS for the bootstrap.  Once
 * dlopen lands (stage 7) the file will also be registered at
 * /lib/ld-kappara.so for symbolic discovery. */
extern char ld_kappara_blob_start[];
extern char ld_kappara_blob_end[];

/* DYNAMIC.md stage 6: libc.so blob -- same path as ld-kappara.so.
 * Loaded by the kernel at LIBC_VA for every ET_DYN exec.  Stage 7
 * will register it at /lib/libc.so once dlopen needs symbolic
 * discovery. */
extern char libc_so_blob_start[];
extern char libc_so_blob_end[];

/* DYNAMIC.md stage 7: tiny test .so for dlopen end-to-end coverage. */
extern char libdltest_blob_start[];
extern char libdltest_blob_end[];

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

	/* /lib -- DYNAMIC.md stages 5/6: shared objects registered as
	 * VFS regular files.  The exec loader doesn't read them via the
	 * VFS (it uses the blob pointers directly), but /lib/libc.so and
	 * /lib/ld-kappara.so being visible lets userland inspect them
	 * (e.g. `cat /lib/libc.so` from the shell) and gives stage 7's
	 * dlopen a name-to-blob mapping path. */
	{
		static struct blob_priv libc_so_priv;
		static struct blob_priv ld_so_priv;
		static struct blob_priv dltest_so_priv;
		libc_so_priv.data = (const unsigned char *)libc_so_blob_start;
		libc_so_priv.size = (size_t)(libc_so_blob_end - libc_so_blob_start);
		ld_so_priv.data   = (const unsigned char *)ld_kappara_blob_start;
		ld_so_priv.size   = (size_t)(ld_kappara_blob_end - ld_kappara_blob_start);
		dltest_so_priv.data = (const unsigned char *)libdltest_blob_start;
		dltest_so_priv.size = (size_t)(libdltest_blob_end - libdltest_blob_start);
		struct dentry *lib = vfs_mkdir(vfs_root(), "lib");
		vfs_mknod_regfile(lib, "libc.so", &blob_fops, &libc_so_priv);
		vfs_mknod_regfile(lib, "ld-kappara.so", &blob_fops, &ld_so_priv);
		vfs_mknod_regfile(lib, "libdltest.so", &blob_fops, &dltest_so_priv);
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
		PAY("nm",         nm_blob_start,         nm_blob_end),
		PAY("ldd",        ldd_blob_start,        ldd_blob_end),
		PAY("objdump",    objdump_blob_start,    objdump_blob_end),
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

/* DYNAMIC.md stage 7: separate read buffer for dlopen so it doesn't
 * race with execve.  64 KB is enough for our largest .so today
 * (libc.so at ~22 KB). */
static unsigned char dlopen_read_buf[64 * 1024];
static spinlock_t dlopen_buf_lock;

/*
 * DYNAMIC.md stage 5: load a static ET_EXEC ELF (today only ld-kappara.so)
 * from a kernel buffer into the given vm_map.  No relocations -- the ELF's
 * p_vaddr is honoured as-is, and the caller validates it falls into the
 * allowed VA window.  Returns e_entry on success, 0 on failure.
 *
 * This is the secondary loader used by sys_execve_impl to map the dynamic
 * linker alongside ET_DYN applications.  The primary PT_LOAD walk in
 * sys_execve_impl handles ET_DYN with relocs; we don't share code yet
 * because that path is already non-trivial.  If a future binary needs
 * relocations from the secondary slot, refactor then.
 */
static uint64_t load_static_elf(struct vm_map *vm,
                                 const unsigned char *img, size_t img_sz,
                                 uint64_t va_min, uint64_t va_max)
{
	if (img_sz < sizeof(Elf64_Ehdr)) return 0;

	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;
	if (eh->e_ident[EI_MAG0] != 0x7f ||
	    eh->e_ident[EI_MAG1] != 'E'  ||
	    eh->e_ident[EI_MAG2] != 'L'  ||
	    eh->e_ident[EI_MAG3] != 'F' ||
	    eh->e_ident[EI_CLASS] != 2 || eh->e_ident[EI_DATA] != 1 ||
	    eh->e_machine != EM_AARCH64 || eh->e_type != ET_EXEC) {
		kprintf("ld-loader: bad ELF (type/class/machine)\n");
		return 0;
	}

	for (unsigned i = 0; i < eh->e_phnum; i++) {
		const Elf64_Phdr *ph = (const Elf64_Phdr *)(img +
				 eh->e_phoff + (uint64_t)i * eh->e_phentsize);
		if (ph->p_type != PT_LOAD) continue;
		if (ph->p_vaddr < va_min ||
		    ph->p_vaddr + ph->p_memsz > va_max) {
			kprintf("ld-loader: LOAD 0x%lx+0x%lx outside [%lx,%lx)\n",
				(unsigned long)ph->p_vaddr,
				(unsigned long)ph->p_memsz,
				(unsigned long)va_min, (unsigned long)va_max);
			return 0;
		}
		uint64_t seg_start = ph->p_vaddr & ~(uint64_t)PAGE_MASK;
		uint64_t seg_end   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1)
		                     & ~(uint64_t)PAGE_MASK;
		unsigned n_pages   = (unsigned)((seg_end - seg_start) / PAGE_SIZE);
		if (vmap_alloc_pages(vm, seg_start, n_pages, /*zero=*/1) < 0) {
			kprintf("ld-loader: PMM exhausted\n");
			return 0;
		}
		if (ph->p_filesz > 0) {
			if (vmap_copyin(vm, ph->p_vaddr,
			                img + ph->p_offset, ph->p_filesz) < 0)
				return 0;
		}
	}
	return eh->e_entry;
}

/* ---- DYNAMIC.md stage 6 helpers ------------------------------------ */

/* Find the in-image (kernel-buffer) pointer for an ELF runtime VA.
 * Walks PT_LOADs of `eh` looking for the one that covers `img_va`
 * (= an address in the binary's own preferred VA space, i.e. with
 *  load_base of 0).  Returns NULL if no PT_LOAD covers it.
 *
 * Used to dereference symtab / strtab / rela tables that DT_* tags
 * point at -- those tags carry image VAs (load-base-relative), and
 * the kernel reads them out of the on-disk image (since the binary
 * is small and elf_read_buf / libc_so_blob have the bytes already). */
static const void *image_to_ptr(const unsigned char *img,
				const Elf64_Ehdr *eh, uint64_t img_va)
{
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		const Elf64_Phdr *ph = (const Elf64_Phdr *)(img +
				eh->e_phoff + (uint64_t)i * eh->e_phentsize);
		if (ph->p_type != PT_LOAD) continue;
		if (img_va >= ph->p_vaddr &&
		    img_va <  ph->p_vaddr + ph->p_filesz) {
			return img + ph->p_offset + (img_va - ph->p_vaddr);
		}
	}
	return NULL;
}

/* Captured layout of an ELF's .dynamic.  Values that are addresses
 * are kept as image VAs (load-base-relative); the caller adds
 * load_base when writing fixups. */
struct dyn_info {
	uint64_t rela_off,   rela_size;	/* DT_RELA   / DT_RELASZ   */
	uint64_t jmprel_off, jmprel_size; /* DT_JMPREL / DT_PLTRELSZ */
	uint64_t symtab_off, strtab_off; /* DT_SYMTAB / DT_STRTAB    */
	uint64_t strsz;			 /* DT_STRSZ -- bounds st_name */
	uint64_t hash_off;		 /* DT_HASH  -- nchain == nsyms */
};

/* Walk PT_DYNAMIC of `eh` and populate `out`.  Returns 0 on success
 * (PT_DYNAMIC found and walked), -1 if absent. */
static int walk_dynamic(const unsigned char *img, const Elf64_Ehdr *eh,
			struct dyn_info *out)
{
	*out = (struct dyn_info){0};
	const Elf64_Phdr *dyn_ph = NULL;
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		const Elf64_Phdr *ph = (const Elf64_Phdr *)(img +
				eh->e_phoff + (uint64_t)i * eh->e_phentsize);
		if (ph->p_type == PT_DYNAMIC) { dyn_ph = ph; break; }
	}
	if (!dyn_ph) return -1;

	const Elf64_Dyn *dyn = (const Elf64_Dyn *)(img + dyn_ph->p_offset);
	unsigned n = (unsigned)(dyn_ph->p_filesz / sizeof(Elf64_Dyn));
	for (unsigned i = 0; i < n; i++) {
		switch (dyn[i].d_tag) {
		case DT_NULL:                          return 0;
		case DT_RELA:     out->rela_off     = dyn[i].d_un; break;
		case DT_RELASZ:   out->rela_size    = dyn[i].d_un; break;
		case DT_JMPREL:   out->jmprel_off   = dyn[i].d_un; break;
		case DT_PLTRELSZ: out->jmprel_size  = dyn[i].d_un; break;
		case DT_SYMTAB:   out->symtab_off   = dyn[i].d_un; break;
		case DT_STRTAB:   out->strtab_off   = dyn[i].d_un; break;
		case DT_STRSZ:    out->strsz        = dyn[i].d_un; break;
		case DT_HASH:     out->hash_off     = dyn[i].d_un; break;
		}
	}
	return 0;
}

/* Apply R_AARCH64_RELATIVE entries from a binary's .rela.dyn to the
 * VM at user VA `load_base`.  Non-RELATIVE entries are SKIPPED (the
 * cross-DSO walk handles JUMP_SLOT/GLOB_DAT separately).
 *
 * `relas_kva` points into the kernel image of the relocation table;
 * caller already found it via image_to_ptr.  Returns 0 on success or
 * -1 if a vmap_copyin fails (= user VA unmapped). */
static int apply_relative_relocs(struct vm_map *vm, uint64_t load_base,
				 const Elf64_Rela *relas, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		uint32_t t = ELF64_R_TYPE(relas[i].r_info);
		if (t != R_AARCH64_RELATIVE) continue;
		uint64_t fixup = load_base + (uint64_t)relas[i].r_addend;
		uint64_t va    = load_base + relas[i].r_offset;
		if (vmap_copyin(vm, va, &fixup, sizeof(fixup)) < 0) {
			kprintf("reloc: RELATIVE target 0x%lx unmapped\n",
				(unsigned long)va);
			return -1;
		}
	}
	return 0;
}

/* Compare a NUL-terminated symbol name `want` against the libc.so
 * string at libc_strtab[st_name].  Returns 1 if equal. */
static int sym_name_eq(const unsigned char *libc_strtab,
		       uint32_t st_name, const char *want)
{
	const char *s = (const char *)libc_strtab + st_name;
	while (*s && *want && *s == *want) { s++; want++; }
	return *s == 0 && *want == 0;
}

/* Return the symbol-table entry count for a .so by reading the SysV
 * hash table header (DT_HASH).  The .hash section starts with
 * { uint32_t nbucket; uint32_t nchain; ... }, where nchain == # symbols.
 * Returns 0 if no hash table is present (caller should fall back to a
 * conservative scan bounded by string-table safety). */
static unsigned dyn_nsyms(const unsigned char *img, const Elf64_Ehdr *eh,
			   const struct dyn_info *dyn)
{
	if (dyn->hash_off == 0) return 0;
	const uint32_t *h = image_to_ptr(img, eh, dyn->hash_off);
	if (!h) return 0;
	return (unsigned)h[1];	/* nchain == nsyms */
}

/* Resolve a symbol name against libc.so's dynsym.  Returns the
 * runtime VA (= LIBC_VA + symbol value) if found, 0 otherwise.
 *
 * Reads libc.so's on-disk image directly; the in-memory copy was
 * mapped by load_pie_elf but we don't need to walk user VAs because
 * the kernel buffer is the source of truth here. */
static uint64_t resolve_in_libc(const char *name,
				const unsigned char *libc_img,
				const Elf64_Ehdr *libc_eh,
				const struct dyn_info *libc_dyn)
{
	const Elf64_Sym *symtab = image_to_ptr(libc_img, libc_eh,
					       libc_dyn->symtab_off);
	const unsigned char *strtab = image_to_ptr(libc_img, libc_eh,
						   libc_dyn->strtab_off);
	if (!symtab || !strtab) return 0;

	unsigned nsyms = dyn_nsyms(libc_img, libc_eh, libc_dyn);
	if (nsyms == 0 || nsyms > 4096) nsyms = 256;	/* defensive cap */
	for (unsigned i = 0; i < nsyms; i++) {
		const Elf64_Sym *s = &symtab[i];
		if (s->st_name == 0) continue;
		if (libc_dyn->strsz && s->st_name >= libc_dyn->strsz) continue;
		if (s->st_shndx == 0)  continue;	/* SHN_UNDEF */
		if (sym_name_eq(strtab, s->st_name, name))
			return LIBC_VA + s->st_value;
	}
	return 0;
}

/* Apply GLOB_DAT and JUMP_SLOT relocs for the application against
 * libc.so's dynsym.  `relas` points to the kernel image of either
 * .rela.dyn (GLOB_DAT entries) or .rela.plt (JUMP_SLOT entries).
 *
 * Returns 0 on success, -1 if a symbol is missing in libc.so or a
 * fixup target is unmapped. */
static int resolve_xdso_relocs(struct vm_map *vm, uint64_t app_base,
			       const Elf64_Rela *relas, unsigned n,
			       const Elf64_Sym *app_symtab,
			       const unsigned char *app_strtab,
			       const unsigned char *libc_img,
			       const Elf64_Ehdr *libc_eh,
			       const struct dyn_info *libc_dyn)
{
	for (unsigned i = 0; i < n; i++) {
		uint32_t t = ELF64_R_TYPE(relas[i].r_info);
		if (t != R_AARCH64_GLOB_DAT && t != R_AARCH64_JUMP_SLOT)
			continue;
		uint32_t sym_idx = ELF64_R_SYM(relas[i].r_info);
		const Elf64_Sym *s = &app_symtab[sym_idx];
		const char *name = (const char *)app_strtab + s->st_name;
		uint64_t resolved = resolve_in_libc(name, libc_img,
						    libc_eh, libc_dyn);
		if (!resolved) {
			kprintf("reloc: unresolved '%s' (type %u)\n",
				name, t);
			return -1;
		}
		/* GLOB_DAT and JUMP_SLOT both write the symbol value at
		 * r_offset; we ignore r_addend (always 0 in practice
		 * for AArch64). */
		uint64_t va = app_base + relas[i].r_offset;
		if (vmap_copyin(vm, va, &resolved, sizeof(resolved)) < 0) {
			kprintf("reloc: target 0x%lx unmapped\n",
				(unsigned long)va);
			return -1;
		}
	}
	return 0;
}

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

	/* Inherit cwd from the calling thread's vm_map (the shell's),
	 * matching POSIX semantics where execve preserves cwd. */
	{
		struct kthread *parent = curthread;
		if (parent && parent->t_proc && parent->t_proc->vm &&
		    parent->t_proc->vm->cwd[0]) {
			const char *src = parent->t_proc->vm->cwd;
			unsigned i = 0;
			for (; i + 1 < sizeof(vm->cwd) && src[i]; i++)
				vm->cwd[i] = src[i];
			vm->cwd[i] = '\0';
		}
	}

	/* (2) Code: walk PT_LOAD, allocate pages, copy bytes.
	 *
	 * For ET_DYN binaries this uses load_base = EXEC_VA so multiple
	 * PT_LOADs that the linker emitted at p_vaddr 0, 0x1000, 0x2000
	 * end up at EXEC_VA + offset.  For ET_EXEC, load_base = 0 so the
	 * (absolute) p_vaddr is used as-is.  PT_DYNAMIC is located by
	 * walk_dynamic() in the (2b) step below; we don't track it here. */
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = (Elf64_Phdr *)(elf_read_buf +
				 eh->e_phoff + (uint64_t)i * eh->e_phentsize);
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

	/* (2b) DYNAMIC.md stage 8: load ld-kappara.so and libc.so for any
	 * ET_DYN binary.  We DO NOT walk PT_DYNAMIC or apply any relocs --
	 * that's ld-kappara.so's job now.  We just copy bytes; ld.so reads
	 * the auxv we build below to find each object's load base, then
	 * walks each .dynamic and writes the fixups itself.
	 *
	 * ld.so is itself ET_EXEC at fixed LD_VA so it needs no relocation
	 * bootstrap.  libc.so is ET_DYN at LIBC_VA and ld.so applies its
	 * RELATIVE relocs from user space. */
	uint64_t ld_entry = 0;
	if (eh->e_type == ET_DYN) {
		const unsigned char *ld_img = (const unsigned char *)ld_kappara_blob_start;
		size_t ld_sz = (size_t)(ld_kappara_blob_end - ld_kappara_blob_start);
		ld_entry = load_static_elf(vm, ld_img, ld_sz,
		                            LD_VA, LD_VA + LD_SIZE);
		if (ld_entry == 0) {
			kprintf("execve: failed to load ld-kappara.so\n");
			vm_map_put(vm);
			return -1;
		}

		/* Load libc.so PT_LOADs at LIBC_VA + p_vaddr.  No reloc walk --
		 * ld.so handles it. */
		const unsigned char *libc_img = (const unsigned char *)libc_so_blob_start;
		const Elf64_Ehdr *libc_eh = (const Elf64_Ehdr *)libc_img;
		if (libc_eh->e_type != ET_DYN || libc_eh->e_machine != EM_AARCH64) {
			kprintf("execve: libc.so blob malformed\n");
			vm_map_put(vm);
			return -1;
		}
		for (unsigned i = 0; i < libc_eh->e_phnum; i++) {
			const Elf64_Phdr *ph = (const Elf64_Phdr *)(libc_img +
					libc_eh->e_phoff +
					(uint64_t)i * libc_eh->e_phentsize);
			if (ph->p_type != PT_LOAD) continue;
			uint64_t va = ph->p_vaddr + LIBC_VA;
			if (va < LIBC_VA || va + ph->p_memsz > LIBC_VA + LIBC_SIZE) {
				kprintf("execve: libc.so LOAD outside window\n");
				vm_map_put(vm);
				return -1;
			}
			uint64_t seg_start = va & ~(uint64_t)PAGE_MASK;
			uint64_t seg_end   = (va + ph->p_memsz + PAGE_SIZE - 1)
			                     & ~(uint64_t)PAGE_MASK;
			unsigned n_pages   = (unsigned)((seg_end - seg_start) / PAGE_SIZE);
			if (vmap_alloc_pages(vm, seg_start, n_pages, 1) < 0) {
				kprintf("execve: PMM exhausted for libc.so\n");
				vm_map_put(vm);
				return -1;
			}
			if (ph->p_filesz > 0) {
				if (vmap_copyin(vm, va, libc_img + ph->p_offset,
				                ph->p_filesz) < 0) {
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

	/* (5) exec stack setup -- write argv/envp/auxv onto the mapped
	 * stack via vmap_copyin (which walks vm's L3 to find each page).
	 *
	 * Stack layout (grows down from EXEC_STACK_TOP), POSIX shape:
	 *   [string data]
	 *   [16-byte alignment pad]
	 *   [auxv AT_NULL  ]  (16 bytes -- {a_type=0, a_val=0})
	 *   [auxv AT_ENTRY ]  (16 bytes -- {a_type=9, a_val=app_entry})
	 *   [envp[0] = NULL]   8 bytes
	 *   [argv[argc] = NUL] 8 bytes
	 *   [argv[argc-1]   ]
	 *   ...
	 *   [argv[0]        ]
	 *   [argc           ]  <-- SP, 16-byte aligned
	 *
	 * The cmd binary's crt0 reads argc / argv from [sp] / [sp+8]
	 * and is auxv-oblivious; ld-kappara.so's _start parses past
	 * envp to find auxv[AT_ENTRY] and jumps there.
	 *
	 * Padding test: total bytes from SP to strings is
	 * 8 (argc) + 8*argc (argv slots) + 8 (argv NUL) + 8 (envp NUL)
	 * + 4 * 16 (auxv: 3 entries + NULL terminator) = 88 + 8*argc.
	 * SP must be 16-aligned, so we pad with 8 bytes when argc is odd. */
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

	/* 16-byte align before the auxv/envp/argv block. */
	sp &= ~(uint64_t)15;

	/* Pad before auxv when argc is even.  Total below SP from strings:
	 * 8(argc) + 8*argc + 8(argv NUL) + 8(envp NUL) + 64 (4 auxv slots)
	 * = 88 + 8*argc.  88 mod 16 = 8, so for SP 16-aligned: need an
	 * extra 8 bytes of padding when 8*argc is also 0 mod 16 (= argc
	 * even). */
	if (!(effective_argc & 1)) {
		sp -= 8;
		uint64_t zero = 0;
		vmap_copyin(vm, sp, &zero, sizeof(zero));
	}

	/* DYNAMIC.md stage 8: auxv lets ld-kappara.so locate every loaded
	 * object.  Order doesn't matter -- ld_main walks all entries; we
	 * just terminate with AT_NULL.  Layout pushed top-down:
	 *   AT_NULL                  | terminator
	 *   AT_ENTRY = app's _start  | where ld.so jumps after relocs
	 *   AT_KAPPARA_LIBC_BASE     | libc.so load_base (0 for ET_EXEC)
	 *   AT_KAPPARA_APP_BASE      | app load_base (0 for ET_EXEC)
	 */
	{
		uint64_t at_null[2] = { 0, 0 };
		sp -= sizeof(at_null);
		vmap_copyin(vm, sp, at_null, sizeof(at_null));
	}
	{
		uint64_t at_entry[2] = { 9 /* AT_ENTRY */,
		                        eh->e_entry + load_base };
		sp -= sizeof(at_entry);
		vmap_copyin(vm, sp, at_entry, sizeof(at_entry));
	}
	{
		uint64_t at_libc[2] = { 0x101 /* AT_KAPPARA_LIBC_BASE */,
		                        (eh->e_type == ET_DYN) ? LIBC_VA : 0 };
		sp -= sizeof(at_libc);
		vmap_copyin(vm, sp, at_libc, sizeof(at_libc));
	}
	{
		uint64_t at_app[2] = { 0x100 /* AT_KAPPARA_APP_BASE */,
		                       (eh->e_type == ET_DYN) ? load_base : 0 };
		sp -= sizeof(at_app);
		vmap_copyin(vm, sp, at_app, sizeof(at_app));
	}

	/* envp terminator (empty environment for now). */
	{
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

	/* For ET_DYN: enter ld-kappara.so first; it parses auxv for
	 * AT_ENTRY and tail-calls the app.  ET_EXEC binaries are
	 * entered directly (no ld.so loaded). */
	struct exec_args *a = kmalloc(sizeof(*a));
	if (!a) { vm_map_put(vm); return -1; }
	a->entry = (ld_entry != 0) ? ld_entry : (eh->e_entry + load_base);
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

/* ---- DYNAMIC.md stage 7: dlopen / dlsym -------------------------- */

/* Shared by dlopen and dlsym: read the shared object into
 * dlopen_read_buf and return its size, or -1.  Holds dlopen_buf_lock
 * for the duration; caller must release.  Path comes pre-copied from
 * user space (kernel pointer). */
static long dlopen_read(const char *path)
{
	long sz = read_file_kernel(path, dlopen_read_buf,
				    sizeof(dlopen_read_buf));
	if (sz < (long)sizeof(Elf64_Ehdr)) {
		kprintf("dlopen: read '%s' failed (%ld bytes)\n", path, sz);
		return -1;
	}
	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)dlopen_read_buf;
	if (eh->e_ident[EI_MAG0] != 0x7f ||
	    eh->e_ident[EI_MAG1] != 'E'  ||
	    eh->e_ident[EI_MAG2] != 'L'  ||
	    eh->e_ident[EI_MAG3] != 'F'  ||
	    eh->e_ident[EI_CLASS] != 2  ||
	    eh->e_ident[EI_DATA]  != 1  ||
	    eh->e_machine != EM_AARCH64 ||
	    eh->e_type != ET_DYN) {
		kprintf("dlopen: '%s' not an AArch64 ET_DYN ELF\n", path);
		return -1;
	}
	return sz;
}

/* Set vm->dlerror_msg.  Embedded buffer; truncates instead of allocating. */
static void dlerror_set(struct vm_map *vm, const char *msg)
{
	unsigned i = 0;
	for (; i + 1 < sizeof(vm->dlerror_msg) && msg[i]; i++)
		vm->dlerror_msg[i] = msg[i];
	vm->dlerror_msg[i] = '\0';
}

uint64_t sys_dlopen_impl(const char *path)
{
	struct kthread *t = curthread;
	if (!t || !t->t_proc || !t->t_proc->vm) return 0;
	struct vm_map *vm = t->t_proc->vm;
	if (vm->heap_brk == 0) {
		/* Init shells (no per-process backing) can't dlopen. */
		return 0;
	}

	/* Cached-handle fast path: same path -> same handle. */
	for (unsigned s = 0; s < DLOPEN_MAX_SLOTS; s++) {
		if (vm->dlopen_slots[s].base != 0 &&
		    kstrcmp(vm->dlopen_slots[s].path, path) == 0)
			return vm->dlopen_slots[s].base;
	}

	/* Find a free slot. */
	int free_slot = -1;
	for (unsigned s = 0; s < DLOPEN_MAX_SLOTS; s++) {
		if (vm->dlopen_slots[s].base == 0) { free_slot = (int)s; break; }
	}
	if (free_slot < 0) {
		dlerror_set(vm, "dlopen: no free slot (DLOPEN_MAX_SLOTS reached)");
		return 0;
	}

	unsigned long flags = spin_lock_irq_save(&dlopen_buf_lock);

	if (dlopen_read(path) < 0) {
		dlerror_set(vm, "dlopen: file read or ELF validation failed");
		spin_unlock_irq_restore(&dlopen_buf_lock, flags);
		return 0;
	}

	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)dlopen_read_buf;
	const uint64_t load_base = DLOPEN_VA + (uint64_t)free_slot * DLOPEN_SLOT_SIZE;
	const uint64_t slot_end  = load_base + DLOPEN_SLOT_SIZE;

	/* Load PT_LOADs at p_vaddr + load_base. */
	for (unsigned i = 0; i < eh->e_phnum; i++) {
		const Elf64_Phdr *ph = (const Elf64_Phdr *)(dlopen_read_buf +
				 eh->e_phoff + (uint64_t)i * eh->e_phentsize);
		if (ph->p_type != PT_LOAD) continue;
		uint64_t va = ph->p_vaddr + load_base;
		if (va < load_base || va + ph->p_memsz > slot_end) {
			dlerror_set(vm, "dlopen: LOAD outside slot window");
			spin_unlock_irq_restore(&dlopen_buf_lock, flags);
			return 0;
		}
		uint64_t seg_start = va & ~(uint64_t)PAGE_MASK;
		uint64_t seg_end   = (va + ph->p_memsz + PAGE_SIZE - 1)
		                     & ~(uint64_t)PAGE_MASK;
		unsigned n_pages   = (unsigned)((seg_end - seg_start) / PAGE_SIZE);
		if (vmap_alloc_pages(vm, seg_start, n_pages, 1) < 0) {
			dlerror_set(vm, "dlopen: PMM exhausted");
			spin_unlock_irq_restore(&dlopen_buf_lock, flags);
			return 0;
		}
		if (ph->p_filesz > 0) {
			if (vmap_copyin(vm, va, dlopen_read_buf + ph->p_offset,
			                ph->p_filesz) < 0) {
				dlerror_set(vm, "dlopen: vmap_copyin failed");
				spin_unlock_irq_restore(&dlopen_buf_lock, flags);
				return 0;
			}
		}
	}

	/* Apply this .so's RELATIVE relocations + resolve cross-DSO
	 * GLOB_DAT / JUMP_SLOT against libc.so's dynsym -- same shape as
	 * sys_execve_impl's (2b) + (2d) for the app, just with a different
	 * load_base.  A dlopen'd .so can now DT_NEEDED libc.so and call
	 * printf / fork / etc.  Cross-DSO against other dlopen'd modules
	 * is not yet supported (would need a registry of currently-loaded
	 * objects). */
	struct dyn_info dyn = {0};
	if (walk_dynamic(dlopen_read_buf, eh, &dyn) == 0) {
		if (dyn.rela_size > 0) {
			const Elf64_Rela *relas = image_to_ptr(dlopen_read_buf, eh,
							       dyn.rela_off);
			if (relas) {
				unsigned n = (unsigned)(dyn.rela_size / sizeof(Elf64_Rela));
				(void)apply_relative_relocs(vm, load_base, relas, n);
			}
		}

		/* Cross-DSO resolution against libc.so (loaded by execve at
		 * LIBC_VA).  Reuses the same helpers execve uses. */
		const unsigned char *libc_img = (const unsigned char *)libc_so_blob_start;
		const Elf64_Ehdr *libc_eh = (const Elf64_Ehdr *)libc_img;
		struct dyn_info libc_dyn = {0};
		if (walk_dynamic(libc_img, libc_eh, &libc_dyn) == 0) {
			const Elf64_Sym *symtab = image_to_ptr(dlopen_read_buf, eh,
							       dyn.symtab_off);
			const unsigned char *strtab = image_to_ptr(dlopen_read_buf, eh,
								   dyn.strtab_off);
			if (symtab && strtab) {
				if (dyn.rela_size > 0) {
					const Elf64_Rela *relas = image_to_ptr(dlopen_read_buf, eh,
									       dyn.rela_off);
					unsigned n = (unsigned)(dyn.rela_size / sizeof(Elf64_Rela));
					(void)resolve_xdso_relocs(vm, load_base, relas, n,
								  symtab, strtab,
								  libc_img, libc_eh, &libc_dyn);
				}
				if (dyn.jmprel_size > 0) {
					const Elf64_Rela *relas = image_to_ptr(dlopen_read_buf, eh,
									       dyn.jmprel_off);
					unsigned n = (unsigned)(dyn.jmprel_size / sizeof(Elf64_Rela));
					(void)resolve_xdso_relocs(vm, load_base, relas, n,
								  symtab, strtab,
								  libc_img, libc_eh, &libc_dyn);
				}
			}
		}
	}

	/* D-cache flush + I-cache invalidate for the new code pages. */
	__asm__ volatile (
		"dsb  ish\n"
		"ic   iallu\n"
		"dsb  ish\n"
		"isb\n"
		::: "memory");

	/* Slot bookkeeping: stash base + path so dlsym / dlclose can find it. */
	vm->dlopen_slots[free_slot].base = load_base;
	{
		unsigned i = 0;
		char *dst = vm->dlopen_slots[free_slot].path;
		for (; i + 1 < sizeof(vm->dlopen_slots[free_slot].path) && path[i]; i++)
			dst[i] = path[i];
		dst[i] = '\0';
	}

	spin_unlock_irq_restore(&dlopen_buf_lock, flags);
	return load_base;
}

/* Locate the slot whose base matches `handle`; -1 if none. */
static int dlopen_slot_of(const struct vm_map *vm, uint64_t handle)
{
	if (handle == 0) return -1;
	for (unsigned s = 0; s < DLOPEN_MAX_SLOTS; s++)
		if (vm->dlopen_slots[s].base == handle) return (int)s;
	return -1;
}

uint64_t sys_dlsym_impl(uint64_t handle, const char *name)
{
	struct kthread *t = curthread;
	if (!t || !t->t_proc || !t->t_proc->vm) return 0;
	struct vm_map *vm = t->t_proc->vm;
	int slot = dlopen_slot_of(vm, handle);
	if (slot < 0) {
		dlerror_set(vm, "dlsym: invalid handle");
		return 0;
	}

	unsigned long flags = spin_lock_irq_save(&dlopen_buf_lock);

	if (dlopen_read(vm->dlopen_slots[slot].path) < 0) {
		dlerror_set(vm, "dlsym: backing file unreadable");
		spin_unlock_irq_restore(&dlopen_buf_lock, flags);
		return 0;
	}

	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)dlopen_read_buf;
	struct dyn_info dyn = {0};
	if (walk_dynamic(dlopen_read_buf, eh, &dyn) < 0) {
		dlerror_set(vm, "dlsym: no PT_DYNAMIC");
		spin_unlock_irq_restore(&dlopen_buf_lock, flags);
		return 0;
	}

	const Elf64_Sym *symtab = image_to_ptr(dlopen_read_buf, eh,
					       dyn.symtab_off);
	const unsigned char *strtab = image_to_ptr(dlopen_read_buf, eh,
						   dyn.strtab_off);
	uint64_t resolved = 0;
	if (symtab && strtab) {
		unsigned nsyms = dyn_nsyms(dlopen_read_buf, eh, &dyn);
		if (nsyms == 0 || nsyms > 4096) nsyms = 256;	/* defensive */
		for (unsigned i = 0; i < nsyms; i++) {
			const Elf64_Sym *s = &symtab[i];
			if (s->st_name == 0) continue;
			if (dyn.strsz && s->st_name >= dyn.strsz) continue;
			if (s->st_shndx == 0) continue;	/* SHN_UNDEF */
			if (sym_name_eq(strtab, s->st_name, name)) {
				resolved = handle + s->st_value;
				break;
			}
		}
	}

	if (!resolved)
		dlerror_set(vm, "dlsym: symbol not found");

	spin_unlock_irq_restore(&dlopen_buf_lock, flags);
	return resolved;
}

/* sys_dlclose -- unmap a previously-dlopen'd slot.  Walks every page
 * in [base, base + DLOPEN_SLOT_SIZE), pmm_free's any backing pages, and
 * clears the slot's bookkeeping.  Returns 0 on success / -1 on bad
 * handle.  Doesn't bother to free L3 tables (still re-usable by another
 * dlopen at the same slot). */
long sys_dlclose_impl(uint64_t handle)
{
	struct kthread *t = curthread;
	if (!t || !t->t_proc || !t->t_proc->vm) return -1;
	struct vm_map *vm = t->t_proc->vm;
	int slot = dlopen_slot_of(vm, handle);
	if (slot < 0) {
		dlerror_set(vm, "dlclose: invalid handle");
		return -1;
	}

	uint64_t base = vm->dlopen_slots[slot].base;
	for (uint64_t va = base; va < base + DLOPEN_SLOT_SIZE; va += PAGE_SIZE)
		(void)mmu_vmap_unmap_user_4k(vm, va);

	/* I-cache invalidate: the slot's code may have been executed and is
	 * now being torn down; a future dlopen at the same slot puts fresh
	 * code there. */
	__asm__ volatile (
		"dsb  ish\n"
		"ic   iallu\n"
		"dsb  ish\n"
		"isb\n"
		::: "memory");

	vm->dlopen_slots[slot].base = 0;
	vm->dlopen_slots[slot].path[0] = '\0';
	return 0;
}

/* sys_dlerror -- copy the current error string to a user buffer.
 * Clears the error after copying (POSIX-style: second call returns 0).
 * Returns the number of bytes copied (excluding NUL), or 0 if no error. */
long sys_dlerror_impl(char *user_buf, unsigned cap)
{
	struct kthread *t = curthread;
	if (!t || !t->t_proc || !t->t_proc->vm) return 0;
	struct vm_map *vm = t->t_proc->vm;
	if (vm->dlerror_msg[0] == '\0') return 0;

	unsigned n = 0;
	while (n + 1 < cap && vm->dlerror_msg[n]) n++;

	char nul = '\0';
	if (syscall_from_user) {
		if (copy_to_user(user_buf, vm->dlerror_msg, n) < 0) return 0;
		if (copy_to_user(user_buf + n, &nul, 1) < 0) return 0;
	} else {
		for (unsigned i = 0; i < n; i++) user_buf[i] = vm->dlerror_msg[i];
		user_buf[n] = '\0';
	}
	vm->dlerror_msg[0] = '\0';
	return (long)n;
}

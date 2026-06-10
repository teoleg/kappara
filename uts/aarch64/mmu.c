/*
 * arch/aarch64/mmu.c -- AArch64 stage-1 MMU bring-up
 * ==================================================
 *
 * What this file is
 * -----------------
 * Builds a simple identity-mapping page-table tree, programs the MMU
 * control registers, then flips on translation + caches.  After
 * mmu_init() returns the CPU is translating every virtual address
 * through these tables (VA == PA for everything we care about) with
 * the D-cache and I-cache enabled.
 *
 * Stage-1 page-table walk (4 KB granule, 48-bit VA)
 * -------------------------------------------------
 * AArch64 splits a 48-bit VA into 9-bit indices per level, plus the
 * 12-bit page offset:
 *
 *     47        39 38        30 29        21 20        12 11      0
 *    +-----------+-------------+-------------+-------------+--------+
 *    |   L0 idx  |   L1 idx    |   L2 idx    |   L3 idx    | offset |
 *    +-----------+-------------+-------------+-------------+--------+
 *         |             |             |             |
 *         v             v             v             v
 *      l0_table     l1_table      l2_table       (page)
 *      [512 x 8]    [512 x 8]     [512 x 8]
 *
 * Each level's entry either:
 *   - points to the next level's table (type=0b11), or
 *   - is a "block" descriptor for a large mapping (type=0b01):
 *       L1 block = 1 GB, L2 block = 2 MB, L3 entry = 4 KB page.
 *
 * Our tree
 * --------
 *
 *     l0_table[0]   -> l1_table          (covers VA 0..512 GB)
 *     l1_table[0]   -> l2_table          (covers VA 0..1 GB)
 *     l1_table[1]   -> 1 GB Device block (covers VA 1..2 GB,
 *                                          BCM2836 local peripherals)
 *     l2_table[i]   -> 2 MB block        (each one covers a 2 MB
 *                                          chunk of the low 1 GB)
 *
 *   For each l2_table[i]:
 *     - 0x3F000000 <= PA < 0x40000000  -> Device-nGnRE (UART, GPIO, ...)
 *     - else                            -> Normal Inner+Outer WBWA,
 *                                          Inner-Shareable, UXN
 *
 * Block descriptor bit layout (2 MB block at L2, 4 KB granule)
 * -----------------------------------------------------------
 *
 *     63  55 54 53 52    47          21 20  12 11 10 9 8 7 6 5 4 3 2 1 0
 *    +-----+--+--+--+------+------------+------+--+-+---+---+---+-+----+--+
 *    | IGN |UX|PX|  | RES0 |  PA[47:21] | RES0 |nG|AF|SH |AP |NS|AttrIdx|V|
 *    +-----+--+--+--+------+------------+------+--+-+---+---+---+-+----+--+
 *                                                              |       block/
 *                                                              |       table
 *                                                              v
 *                                                          bit 1 = 0
 *
 *   V         = valid                     (1)
 *   AttrIdx   = MAIR_EL1 byte selector    (0 = Normal, 2 = Device for us)
 *   NS        = non-secure                (irrelevant at our EL)
 *   AP[2:1]   = access permissions        (00 = RW EL1, no EL0)
 *   SH[1:0]   = shareability              (11 = Inner Shareable for Normal,
 *                                          00 = ignored for Device)
 *   AF        = access flag               (must be 1; we set it everywhere)
 *   nG        = non-global                (0 for kernel mappings)
 *   PA[47:21] = physical block base
 *   PXN/UXN   = no privileged/unpriv exec (for Device and UXN for Normal)
 *
 * MAIR_EL1 -- memory attribute lookup table
 * -----------------------------------------
 *   Eight one-byte slots, indexed by AttrIdx in a descriptor.  We
 *   define just two:
 *
 *     idx 0  Normal memory, Outer WBWA + Inner WBWA cacheable    = 0xFF
 *     idx 2  Device-nGnRE                                         = 0x04
 *
 *   MAIR_EL1 byte layout: 0xFF means "Outer WB, R+W alloc; Inner WB,
 *   R+W alloc" -- the most aggressive cacheable mode, fine for RAM.
 *
 * TCR_EL1 -- translation control register
 * ---------------------------------------
 *   T0SZ=16        VA size = 64-16 = 48 bits        (whole TTBR0 region)
 *   IRGN0/ORGN0=01 inner/outer WBWA for the table walker itself
 *   SH0=11         table walks are inner-shareable
 *   TG0=00         4 KB granule
 *   EPD1=1         disable TTBR1 walks (we don't use the high half yet)
 *   TG1=10         4 KB for TTBR1 (moot with EPD1=1, but encoded)
 *   IPS=001        36-bit output physical address space
 *
 * The MMU enable dance
 * --------------------
 *   1. Write MAIR_EL1, TCR_EL1, TTBR0_EL1   (config first)
 *   2. tlbi vmalle1; dsb sy; isb            (flush any stale TLB)
 *   3. ic iallu; dsb sy                     (flush I-cache as a paranoid
 *                                              measure; nothing in I-cache
 *                                              has been written by us)
 *   4. Set SCTLR_EL1.{M,C,I} and write back (this is the actual switch)
 *   5. isb                                  (synchronise the new pipe)
 *
 *   After the ISB at step 5, every memory access is translated and
 *   cacheable RAM goes through L1 caches.  Because our mapping is
 *   identity, every pointer that was valid before is still valid.
 */

#include <stdint.h>

#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"
#include "platform.h"

#define ENTRIES_PER_TABLE	512

/* MAIR_EL1 attribute indices. */
#define ATTR_NORMAL_IDX		0
#define ATTR_DEVICE_IDX		2

/* Memory attribute encodings stored in MAIR_EL1 byte fields. */
#define MAIR_NORMAL_WBWA	0xFFUL	/* Inner+Outer WB, R+W allocate */
#define MAIR_DEVICE_NGNRE	0x04UL	/* Device-nGnRE */

#define MAIR_VALUE \
	((MAIR_DEVICE_NGNRE << (ATTR_DEVICE_IDX * 8)) | \
	 (MAIR_NORMAL_WBWA  << (ATTR_NORMAL_IDX * 8)))

/* TCR_EL1 fields. */
#define TCR_T0SZ(n)		((uint64_t)((n) & 0x3F))
#define TCR_IRGN0_WBWA		(1UL << 8)
#define TCR_ORGN0_WBWA		(1UL << 10)
#define TCR_SH0_INNER		(3UL << 12)
#define TCR_TG0_4K		(0UL << 14)
#define TCR_EPD1		(1UL << 23)	/* disable TTBR1 walks */
#define TCR_TG1_4K		(2UL << 30)
#define TCR_IPS_36BIT		(1UL << 32)

#define TCR_VALUE \
	(TCR_T0SZ(16) | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | TCR_SH0_INNER | \
	 TCR_TG0_4K | TCR_EPD1 | TCR_TG1_4K | TCR_IPS_36BIT)

/* Stage 1 descriptor bits (block / table at any level). */
#define D_VALID			(1UL << 0)
#define D_TABLE			(1UL << 1)	/* 1=table, 0=block at L0..L2 */
#define D_ATTRIDX(n)		(((uint64_t)(n) & 0x7) << 2)
#define D_AP_RW_EL1		(0UL << 6)	/* AP[2:1]=00: RW EL1, no EL0 */
#define D_AP_RW_EL0		(1UL << 6)	/* AP[2:1]=01: RW EL1+EL0    */
#define D_SH_INNER		(3UL << 8)
#define D_SH_NONE		(0UL << 8)
#define D_AF			(1UL << 10)
#define D_PXN			(1UL << 53)
#define D_UXN			(1UL << 54)

/* 2 MB block at L2. */
#define BLOCK_2M_SHIFT		21
#define BLOCK_2M_SIZE		(1UL << BLOCK_2M_SHIFT)

#define PERIPH_BASE		PLAT_PERIPH_BASE
#define PERIPH_END		PLAT_PERIPH_END

/* SCTLR_EL1 bits we flip on. */
#define SCTLR_M			(1UL << 0)
#define SCTLR_C			(1UL << 2)
#define SCTLR_I			(1UL << 12)

__attribute__((aligned(PAGE_SIZE))) static uint64_t l0_table[ENTRIES_PER_TABLE];

unsigned long mmu_boot_l0_phys(void)
{
	return (unsigned long)(uintptr_t)l0_table;
}
__attribute__((aligned(PAGE_SIZE))) static uint64_t l1_table[ENTRIES_PER_TABLE];
__attribute__((aligned(PAGE_SIZE))) static uint64_t l2_table[ENTRIES_PER_TABLE];

static void build_identity_map(void)
{
	l0_table[0] = (uint64_t)(uintptr_t)l1_table | D_VALID | D_TABLE;
	l1_table[0] = (uint64_t)(uintptr_t)l2_table | D_VALID | D_TABLE;

	/*
	 * L1[1]: identity-map VA 0x40000000..0x80000000 (1 GB block) as Device.
	 * This covers the BCM2836 ARM-local peripheral window (timer routing,
	 * mailboxes, per-core IRQ source) starting at 0x40000000.  Coarse but
	 * cheap; only the first ~1 MB above 0x40000000 is actually populated.
	 */
	l1_table[1] = PLAT_LOCAL_PERIPH_BASE |
		      D_VALID | D_ATTRIDX(ATTR_DEVICE_IDX) |
		      D_AP_RW_EL1 | D_SH_NONE | D_AF | D_PXN | D_UXN;

	for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
		uint64_t pa = (uint64_t)i << BLOCK_2M_SHIFT;
		uint64_t desc;

		if (pa >= PERIPH_BASE && pa < PERIPH_END) {
			desc = pa | D_VALID | D_ATTRIDX(ATTR_DEVICE_IDX) |
			       D_AP_RW_EL1 | D_SH_NONE | D_AF |
			       D_PXN | D_UXN;
		} else {
			desc = pa | D_VALID | D_ATTRIDX(ATTR_NORMAL_IDX) |
			       D_AP_RW_EL1 | D_SH_INNER | D_AF |
			       D_UXN;
		}
		l2_table[i] = desc;
	}
}

/*
 * Remap one 2 MB L2 block to make it accessible from EL0.
 *
 *   va, pa     must be 2 MB aligned, and va must lie in the lowest
 *              1 GB (the L2 we built in build_identity_map covers
 *              VA 0..1 GB).
 *
 * The new descriptor is Normal cacheable, inner-shareable, AF set,
 * AP[2:1]=01 (RW for both EL1 and EL0).  We leave UXN clear so user
 * code can be executed; PXN is also clear so the kernel can
 * trampoline into and out of the page if needed.
 *
 * After updating the descriptor we TLBI the VA and ISB so subsequent
 * accesses go through the new mapping.  The data we wrote into the
 * region beforehand (if any) is cleaned + invalidated by the caller
 * via cache maintenance (DC CVAU + IC IVAU) -- needed when populating
 * a user code page so the I-cache doesn't keep stale lines.
 */
void mmu_map_user_2mb(uint64_t va, uint64_t pa)
{
	unsigned idx = (unsigned)(va >> BLOCK_2M_SHIFT) & 0x1ff;
	l2_table[idx] = pa | D_VALID | D_ATTRIDX(ATTR_NORMAL_IDX) |
			D_AP_RW_EL0 | D_SH_INNER | D_AF;

	__asm__ volatile (
		"dsb	ishst\n"
		"tlbi	vaae1is, %0\n"
		"dsb	ish\n"
		"isb\n"
		: : "r"(va >> 12) : "memory");
}

/* Per-CPU MMU enable: program the SYSREGs from the same shared
 * page tables core 0 built, then turn the MMU on.  Secondary cores
 * call this from their EL1 entry after dropping from EL2.  No
 * page-table builds here; that's a once-per-boot step done from
 * mmu_init on core 0. */
void mmu_enable_this_cpu(void)
{
	uint64_t ttbr0 = (uint64_t)(uintptr_t)l0_table;

	__asm__ volatile (
		"msr	mair_el1,  %0\n"
		"msr	tcr_el1,   %1\n"
		"msr	ttbr0_el1, %2\n"
		"isb\n"
		"tlbi	vmalle1\n"
		"dsb	sy\n"
		"isb\n"
		:
		: "r"((uint64_t)MAIR_VALUE),
		  "r"((uint64_t)TCR_VALUE),
		  "r"(ttbr0)
		: "memory");

	uint64_t sctlr;
	__asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;

	__asm__ volatile (
		"ic	iallu\n"
		"dsb	sy\n"
		"msr	sctlr_el1, %0\n"
		"isb\n"
		:
		: "r"(sctlr)
		: "memory");
}

void mmu_init(void)
{
	build_identity_map();
	mmu_enable_this_cpu();
	kprintf("mmu: enabled (ttbr0=0x%lx tcr=0x%lx mair=0x%lx)\n",
		(unsigned long)(uintptr_t)l0_table,
		(unsigned long)TCR_VALUE,
		(unsigned long)MAIR_VALUE);
}

/* ---- Phase R1c2: per-process vm_map page tables ----------------------- */

/*
 * Each process gets its own L0 + L1 + L2 trinity.  The L0[0] entry
 * points at the process's private L1; L1[0] points at the private L2
 * (covering the low 1 GB) and L1[1] is the shared peripheral mapping
 * carried over from boot.  L2 starts as a verbatim copy of the boot
 * L2 -- which gives the process the same identity-mapped kernel
 * region -- then sys_execve later overwrites the USER_VA L2 slot to
 * point at this process's user code/data.
 *
 * Memory cost per process: 3 x 4 KiB = 12 KiB of page-table state,
 * pmm-allocated, freed on process exit.
 *
 * The kernel runs in low-VA today, so EVERY process must keep the
 * full kernel mapping live in its L2 -- otherwise switching to it
 * would cause the kernel to fault on its own code.  When the kernel
 * eventually moves to TTBR1_EL1 (high VA), per-process L2 will only
 * need to cover user range.  Until then we eat the duplication.
 */

#include "kappara/process.h"

/* Walk every L2 entry of the boot table and copy it into the new
 * process's L2.  After this the process can run kernel code through
 * the same identity mapping the boot CPU uses. */
static void vmap_dup_kernel_l2(uint64_t *dst_l2)
{
	for (int i = 0; i < ENTRIES_PER_TABLE; i++)
		dst_l2[i] = l2_table[i];
}

int mmu_vmap_create(struct vm_map *vm)
{
	if (!vm) return -1;

	void *l0 = pmm_alloc();
	void *l1 = pmm_alloc();
	void *l2 = pmm_alloc();
	if (!l0 || !l1 || !l2) {
		if (l0) pmm_free(l0);
		if (l1) pmm_free(l1);
		if (l2) pmm_free(l2);
		return -1;
	}

	uint64_t *l0_va = (uint64_t *)l0;
	uint64_t *l1_va = (uint64_t *)l1;
	uint64_t *l2_va = (uint64_t *)l2;

	/* pmm returns zero pages, so unused entries are already 0
	 * (invalid).  Just wire the three levels and copy the boot
	 * L2 in. */
	l0_va[0] = (uint64_t)(uintptr_t)l1 | D_VALID | D_TABLE;
	l1_va[0] = (uint64_t)(uintptr_t)l2 | D_VALID | D_TABLE;
	/* Inherit the peripheral 1 GB mapping at L1[1] from boot --
	 * the kernel touches PL011 UART + GPIO + mailboxes through
	 * here and would fault on switch otherwise. */
	l1_va[1] = l1_table[1];

	vmap_dup_kernel_l2(l2_va);

	vm->l0_phys = (uint64_t)(uintptr_t)l0;
	vm->l1_phys = (uint64_t)(uintptr_t)l1;
	vm->l2_phys = (uint64_t)(uintptr_t)l2;
	return 0;
}

/* Defined further down with the rest of the 4 KB-mapping code. */
static void vmap_free_user_l3s(uint64_t *l2);

void mmu_vmap_destroy(struct vm_map *vm)
{
	if (!vm) return;
	if (vm->l2_phys) {
		/* Walk L2 for user L3 tables; free user pages + the L3
		 * itself.  Kernel BLOCK entries are skipped (D_TABLE off). */
		vmap_free_user_l3s((uint64_t *)(uintptr_t)vm->l2_phys);
	}
	if (vm->l0_phys) pmm_free((void *)(uintptr_t)vm->l0_phys);
	if (vm->l1_phys) pmm_free((void *)(uintptr_t)vm->l1_phys);
	if (vm->l2_phys) pmm_free((void *)(uintptr_t)vm->l2_phys);
	vm->l0_phys = vm->l1_phys = vm->l2_phys = 0;
}

/* Install a 2 MB user-RW block at `va` -> `pa` in `vm`'s L2.  Same
 * descriptor format mmu_map_user_2mb uses on the boot table.  No
 * TLBI is issued because the new mapping isn't live until something
 * later writes TTBR0_EL1 = vm->l0_phys. */
void mmu_vmap_map_user_2mb(struct vm_map *vm, uint64_t va, uint64_t pa)
{
	if (!vm || !vm->l2_phys) return;
	uint64_t *l2 = (uint64_t *)(uintptr_t)vm->l2_phys;
	unsigned idx = (unsigned)(va >> BLOCK_2M_SHIFT) & 0x1ff;
	l2[idx] = pa | D_VALID | D_ATTRIDX(ATTR_NORMAL_IDX) |
		  D_AP_RW_EL0 | D_SH_INNER | D_AF;
}

/* ---- R6: 4 KB user page mappings via L3 tables -------------------- */

/* Mask of PA bits in a descriptor: bits 12..47 (4 KB-aligned, 48-bit PA). */
#define D_PA_MASK	0x0000fffffffff000UL

/* Get or allocate the L3 table covering `va` in `vm`.  Converts the
 * L2 entry from a (possibly-stale kernel) BLOCK into a TABLE pointing
 * at the fresh L3 the first time the 2 MB region is touched.  Returns
 * the L3's kernel VA (== PA, since kernel is identity-mapped), or
 * NULL on PMM exhaustion. */
static uint64_t *vmap_l3_for(struct vm_map *vm, uint64_t va)
{
	if (!vm || !vm->l2_phys) return NULL;
	uint64_t *l2 = (uint64_t *)(uintptr_t)vm->l2_phys;
	unsigned idx = (unsigned)(va >> BLOCK_2M_SHIFT) & 0x1ff;
	uint64_t e = l2[idx];
	if ((e & D_VALID) && (e & D_TABLE)) {
		/* Already pointing at an L3 we installed earlier. */
		return (uint64_t *)(uintptr_t)(e & D_PA_MASK);
	}
	void *l3 = pmm_alloc();
	if (!l3) return NULL;
	/* pmm_alloc returns zeroed pages, so all L3 entries start
	 * invalid -- nothing pmm_free's stale data into the wrong VA. */
	l2[idx] = (uint64_t)(uintptr_t)l3 | D_VALID | D_TABLE;
	return (uint64_t *)l3;
}

/* Install a 4 KB user-RW page at `va` -> `pa` in `vm`.  Allocates an
 * L3 covering the surrounding 2 MB region on first use.  Returns 0
 * on success, -1 on PMM exhaustion.  No TLBI here; the new mapping
 * goes live the next time vm's TTBR0 is loaded. */
int mmu_vmap_map_user_4k(struct vm_map *vm, uint64_t va, uint64_t pa)
{
	uint64_t *l3 = vmap_l3_for(vm, va);
	if (!l3) return -1;
	unsigned idx = (unsigned)(va >> PAGE_SHIFT) & 0x1ff;
	/* D_TABLE bit at L3 is the AArch64 "page descriptor" marker
	 * -- it MUST be set on a valid page entry.  An L3 entry with
	 * D_TABLE clear is treated as a reserved descriptor and faults. */
	l3[idx] = (pa & D_PA_MASK) | D_VALID | D_TABLE |
		  D_ATTRIDX(ATTR_NORMAL_IDX) |
		  D_AP_RW_EL0 | D_SH_INNER | D_AF;
	return 0;
}

/* Translate a user VA into the kernel-side identity address (which
 * is the same as the PA since the kernel is identity-mapped) for
 * direct kernel access to the user page.  Returns NULL if the VA
 * has no L3 mapping installed.
 *
 * Used by sys_execve to write argv onto the user stack and by
 * sys_fork to walk parent pages when copying. */
void *mmu_vmap_user_va_to_kva(const struct vm_map *vm, uint64_t va)
{
	if (!vm || !vm->l2_phys) return NULL;
	uint64_t *l2 = (uint64_t *)(uintptr_t)vm->l2_phys;
	unsigned l2_idx = (unsigned)(va >> BLOCK_2M_SHIFT) & 0x1ff;
	uint64_t e2 = l2[l2_idx];
	if (!(e2 & D_VALID)) return NULL;
	if (!(e2 & D_TABLE)) {
		/* 2 MB block (legacy path).  Direct PA + offset. */
		uint64_t pa = e2 & D_PA_MASK;
		return (void *)(uintptr_t)(pa + (va & (BLOCK_2M_SIZE - 1)));
	}
	uint64_t *l3 = (uint64_t *)(uintptr_t)(e2 & D_PA_MASK);
	unsigned l3_idx = (unsigned)(va >> PAGE_SHIFT) & 0x1ff;
	uint64_t e3 = l3[l3_idx];
	if (!(e3 & D_VALID)) return NULL;
	uint64_t pa = e3 & D_PA_MASK;
	return (void *)(uintptr_t)(pa + (va & PAGE_MASK));
}

/* Unmap a single 4 KB user page from `vm` and pmm_free the backing
 * page.  No-op if the VA isn't mapped.  Returns the PA that was
 * freed, or 0. */
uint64_t mmu_vmap_unmap_user_4k(struct vm_map *vm, uint64_t va)
{
	if (!vm || !vm->l2_phys) return 0;
	uint64_t *l2 = (uint64_t *)(uintptr_t)vm->l2_phys;
	unsigned l2_idx = (unsigned)(va >> BLOCK_2M_SHIFT) & 0x1ff;
	uint64_t e2 = l2[l2_idx];
	if (!(e2 & D_VALID) || !(e2 & D_TABLE)) return 0;
	uint64_t *l3 = (uint64_t *)(uintptr_t)(e2 & D_PA_MASK);
	unsigned l3_idx = (unsigned)(va >> PAGE_SHIFT) & 0x1ff;
	uint64_t e3 = l3[l3_idx];
	if (!(e3 & D_VALID)) return 0;
	uint64_t pa = e3 & D_PA_MASK;
	l3[l3_idx] = 0;
	pmm_free((void *)(uintptr_t)pa);
	return pa;
}

/* Walk vm's L2.  For every entry that's a TABLE in a user-range
 * slot (everything below the kernel L1[1] window), free the L3's
 * page-pointed-at user pages then the L3 itself.  Called from
 * mmu_vmap_destroy.  Boot L2 entries (kernel identity mappings) are
 * BLOCKS, not TABLES, so they're naturally skipped. */
static void vmap_free_user_l3s(uint64_t *l2)
{
	for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
		uint64_t e = l2[i];
		if (!(e & D_VALID) || !(e & D_TABLE))
			continue;
		uint64_t *l3 = (uint64_t *)(uintptr_t)(e & D_PA_MASK);
		for (int j = 0; j < ENTRIES_PER_TABLE; j++) {
			uint64_t pe = l3[j];
			if (!(pe & D_VALID)) continue;
			uint64_t pa = pe & D_PA_MASK;
			pmm_free((void *)(uintptr_t)pa);
		}
		pmm_free(l3);
		l2[i] = 0;
	}
}

/* Set the current CPU's TTBR0_EL1 to `vm`'s L0 and flush the local
 * TLB.  Called from switch_to_next when crossing a process
 * boundary; safe to call from any IRQ-masked kernel context. */
void mmu_vmap_switch(const struct vm_map *vm)
{
	if (!vm) return;
	__asm__ volatile (
		"msr	ttbr0_el1, %0\n"
		"isb\n"
		"tlbi	vmalle1\n"
		"dsb	ish\n"
		"isb\n"
		: : "r"(vm->l0_phys) : "memory");
}

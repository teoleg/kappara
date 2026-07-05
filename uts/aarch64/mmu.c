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

#include "kappara/arch/mmu.h"
#include "kappara/core/pmm.h"
#include "kappara/core/printk.h"
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
#define TCR_IPS_40BIT		(2UL << 32)	/* 40 bits = 1 TB PA */
#define TCR_IPS_44BIT		(4UL << 32)	/* 44 bits = 16 TB PA;
						 * QEMU virt with highmem
						 * PCIe puts ECAM at
						 * 0x4010000000 (>256 GB)
						 * which a 36-bit PA window
						 * cannot reach. */

#define TCR_VALUE \
	(TCR_T0SZ(16) | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | TCR_SH0_INNER | \
	 TCR_TG0_4K | TCR_EPD1 | TCR_TG1_4K | TCR_IPS_44BIT)

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
/* L2 for the second 1 GB (VA 0x40000000–0x7FFFFFFF, where the kernel
 * lives on QEMU virt + AWS Graviton).  Using a TABLE at L1[1] instead
 * of a 1 GB Device block keeps the RAM 2 MB entries as Normal while
 * still letting us carve a single 2 MB Device block for a UEFI UART
 * whose SPCR address falls in this range (e.g. 0x60000000). */
__attribute__((aligned(PAGE_SIZE))) static uint64_t l2_table_hi[ENTRIES_PER_TABLE];
/* Static L1 reserved for the UART region on UEFI paths where the
 * SPCR base is outside the first 512 GB (L0[0]).  Cannot use PMM
 * here — mmu_init() runs before pmm_init(). */
__attribute__((aligned(PAGE_SIZE))) static uint64_t l1_table_uart[ENTRIES_PER_TABLE];

static void build_identity_map(void)
{
	l0_table[0] = (uint64_t)(uintptr_t)l1_table | D_VALID | D_TABLE;
	l1_table[0] = (uint64_t)(uintptr_t)l2_table | D_VALID | D_TABLE;

#if defined(PLATFORM_VIRT)
	/*
	 * QEMU virt / AWS Graviton: RAM lives in the second 1 GB
	 * starting at 0x40000000.  Use a TABLE descriptor pointing at
	 * l2_table_hi (2 MB blocks) rather than a 1 GB Device block so
	 * that mmu_init can later carve out a single 2 MB Device slot
	 * for a UEFI UART whose SPCR address falls in this range
	 * without clobbering the Normal RAM mapping that covers the
	 * kernel text.  A 1 GB Device block with PXN set (the only
	 * alternative) would make the kernel text non-executable the
	 * moment the MMU switched to our tables.
	 */
	l1_table[1] = (uint64_t)(uintptr_t)l2_table_hi | D_VALID | D_TABLE;
	for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
		uint64_t pa = 0x40000000UL + ((uint64_t)i << BLOCK_2M_SHIFT);
		l2_table_hi[i] = pa | D_VALID | D_ATTRIDX(ATTR_NORMAL_IDX) |
				 D_AP_RW_EL1 | D_SH_INNER | D_AF | D_UXN;
	}
#else
	/*
	 * L1[1]: identity-map VA 0x40000000..0x80000000 (1 GB block) as Device.
	 * This covers the BCM2836 ARM-local peripheral window (timer routing,
	 * mailboxes, per-core IRQ source) starting at 0x40000000.  Coarse but
	 * cheap; only the first ~1 MB above 0x40000000 is actually populated.
	 */
	l1_table[1] = PLAT_LOCAL_PERIPH_BASE |
		      D_VALID | D_ATTRIDX(ATTR_DEVICE_IDX) |
		      D_AP_RW_EL1 | D_SH_NONE | D_AF | D_PXN | D_UXN;
#endif

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

/*
 * Identity-map a 1 GB Device block (kernel-only RW, no exec).  The
 * address is rounded down to the 1 GB boundary; on return the
 * containing 1 GB window is reachable from EL1 as MMIO.
 *
 * Used for high-mem MMIO regions that fall outside the 0..2 GB chunk
 * build_identity_map covers via L1[0] (low 1 GB through L2) and L1[1]
 * (RAM at 0x40000000): e.g. QEMU virt's PCIe ECAM at 0x4010000000,
 * NVMe BAR0 at 0x8000000000 (= 512 GB = the very start of L0[1]),
 * or any AWS Graviton MCFG entry above 2 GB.
 *
 * Walks the table tree to whatever L1 the VA wants.  L0[0] reuses
 * the static l1_table that build_identity_map already wired in;
 * L0[1+] entries lazily allocate a fresh L1 page from the PMM
 * (callers run mmu_map_device_1gb AFTER pmm_init -- pcie_init and
 * nvme_init both do).  TLBI vmalle1is is the broadest invalidate;
 * cheap because this only runs at platform init time.
 */
#define BLOCK_1G_SHIFT		30
#define BLOCK_1G_SIZE		(1UL << BLOCK_1G_SHIFT)
#define BLOCK_1G_MASK		(BLOCK_1G_SIZE - 1)
#define BLOCK_512G_SHIFT	39

void mmu_map_device_1gb(uint64_t va)
{
	uint64_t base    = va & ~BLOCK_1G_MASK;
	unsigned l0_idx  = (unsigned)(base >> BLOCK_512G_SHIFT) & 0x1ff;
	unsigned l1_idx  = (unsigned)(base >> BLOCK_1G_SHIFT)   & 0x1ff;

	uint64_t *l1;
	if (l0_idx == 0) {
		l1 = l1_table;
	} else {
		uint64_t l0_desc = l0_table[l0_idx];
		if (l0_desc & D_VALID) {
			/* Reuse existing L1 attached to this L0 entry.
			 * Mask off the descriptor's attribute bits to
			 * recover the L1 phys (= VA, identity). */
			l1 = (uint64_t *)(uintptr_t)(l0_desc & ~0xfffUL);
		} else {
			l1 = pmm_alloc();
			if (!l1) return;
			for (int i = 0; i < ENTRIES_PER_TABLE; i++)
				l1[i] = 0;
			l0_table[l0_idx] = (uint64_t)(uintptr_t)l1
			                   | D_VALID | D_TABLE;
		}
	}

	l1[l1_idx] = base | D_VALID |
		     D_ATTRIDX(ATTR_DEVICE_IDX) |
		     D_AP_RW_EL1 | D_SH_NONE | D_AF | D_PXN | D_UXN;

	__asm__ volatile (
		"dsb	ishst\n"
		"tlbi	vmalle1is\n"
		"dsb	ish\n"
		"isb\n"
		::: "memory");
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
		/* ARM DDI0487 §D8.2.3: all page-table stores must be
		 * observable to the walker before TTBR0_EL1 is written.
		 * QEMU TCG serialises implicitly; real Graviton N1/V1
		 * cores have out-of-order L2 write queues that don't. */
		"dsb	sy\n"
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

	/* Wire the SPCR UART into our page tables so uart_putc stays
	 * valid after mmu_enable_this_cpu() switches to our tables.
	 * Three cases, ordered by the 1 GB block the UART falls in:
	 *
	 *   l0i==0, l1i==0 (0x00000000–0x3FFFFFFF): l1_table[0] is a
	 *   TABLE pointing at l2_table.  Update the 2 MB slot in l2_table
	 *   that contains the UART.  Never touch l1_table[0] itself --
	 *   overwriting it with a Device block makes all of 0–1 GB
	 *   Device/non-executable, which kills user code at 0x10000000.
	 *
	 *   l0i==0, l1i==1 (0x40000000–0x7FFFFFFF): this is the kernel
	 *   RAM range.  Use l2_table_hi (already wired at L1[1]) and mark
	 *   only the 2 MB block that contains the UART as Device.
	 *
	 *   l0i!=0 (above first 512 GB): use the static l1_table_uart
	 *   so we don't need PMM. */
	extern uint64_t efi_uart_base;
	if (efi_uart_base != 0) {
		uint64_t base  = efi_uart_base & ~BLOCK_1G_MASK;
		unsigned l0i   = (unsigned)(base >> BLOCK_512G_SHIFT) & 0x1ff;
		unsigned l1i   = (unsigned)(base >> BLOCK_1G_SHIFT)   & 0x1ff;

		if (l0i == 0 && l1i == 1) {
			/* UART in 1–2 GB (kernel RAM range): use l2_table_hi
			 * at 2 MB granularity so Normal RAM blocks in that 1 GB
			 * window remain intact. */
			unsigned l2i = (unsigned)(efi_uart_base >> BLOCK_2M_SHIFT)
			               & (ENTRIES_PER_TABLE - 1);
			l2_table_hi[l2i] =
				(efi_uart_base & ~(BLOCK_2M_SIZE - 1)) |
				D_VALID | D_ATTRIDX(ATTR_DEVICE_IDX) |
				D_AP_RW_EL1 | D_SH_NONE | D_AF | D_PXN | D_UXN;
		} else if (l0i == 0) {
			/* UART in 0–1 GB: l1_table[0] is a TABLE pointing at
			 * l2_table; updating the 2 MB slot there preserves all
			 * other L2 mappings (including user code at 0x10000000).
			 * Overwriting l1_table[0] with a Device block would make
			 * the whole 0–1 GB range Device/non-executable. */
			unsigned l2i = (unsigned)(efi_uart_base >> BLOCK_2M_SHIFT)
			               & (ENTRIES_PER_TABLE - 1);
			l2_table[l2i] =
				(efi_uart_base & ~(BLOCK_2M_SIZE - 1)) |
				D_VALID | D_ATTRIDX(ATTR_DEVICE_IDX) |
				D_AP_RW_EL1 | D_SH_NONE | D_AF | D_PXN | D_UXN;
		} else {
			/* UART above 2 GB: wire a fresh L1 under the L0 entry. */
			l0_table[l0i] = (uint64_t)(uintptr_t)l1_table_uart
			                | D_VALID | D_TABLE;
			l1_table_uart[l1i] = base | D_VALID |
			               D_ATTRIDX(ATTR_DEVICE_IDX) |
			               D_AP_RW_EL1 | D_SH_NONE | D_AF |
			               D_PXN | D_UXN;
		}
	}

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

#include "kappara/proc/process.h"

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

	/* Inherit every high-mem L0[1+] entry the boot tables grew at
	 * platform-init time (mmu_map_device_1gb populates these for
	 * NVMe BAR0 at 0x8000000000 and any other peripheral above
	 * 512 GB).  Without this, kernel code reached via syscall from
	 * an exec'd thread translates through the per-process L0
	 * we just built and hits an invalid descriptor when poking
	 * those MMIO registers -- canonical symptom: data abort in
	 * nvme_io_submit_and_wait's doorbell write the first time the
	 * user runs `cat /home/<file>`. */
	for (int i = 1; i < ENTRIES_PER_TABLE; i++)
		l0_va[i] = l0_table[i];

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
	/* If the L2 entry is a 2 MB BLOCK, it's the inherited kernel
	 * identity mapping (every fresh vm_map dups l2_table whose
	 * entries are kernel BLOCKs).  User pages always go through an
	 * L3 TABLE installed by vmap_l3_for / mmu_vmap_map_user_4k.  So
	 * BLOCK means "not a user mapping" -- return NULL so fork's
	 * page-walker and friends don't try to clone kernel memory.
	 *
	 * Pre-INDIE.md stage 2, EXEC_VA fit in one 2 MB chunk that
	 * always got TABLE-promoted by the first allocation, so this
	 * fallback was harmless.  Now that EXEC_VA spans 16 MB the
	 * untouched chunks stayed BLOCK and looked "mapped". */
	if (!(e2 & D_TABLE)) return NULL;
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

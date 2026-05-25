/*
 * arch/arm/mmu.c -- ARMv7-A LPAE MMU bring-up
 * ===========================================
 *
 * What this file is
 * -----------------
 * The ARMv7 cousin of arch/aarch64/mmu.c.  Builds an identity map of
 * the whole 32-bit address space and turns on translation + caches,
 * using the LPAE (Large Physical Address Extension) long-format
 * descriptors so the concepts stay close to what we already wrote
 * for AArch64.
 *
 * Why LPAE instead of the classic short-format MMU?
 * -------------------------------------------------
 * ARMv7 has two MMU formats:
 *
 *   Short format  - 32-bit descriptors, 1-level for 1 MB sections or
 *                   2-level for 4 KB pages.  TEX/CB encoding for the
 *                   memory type, AP[2:0] for permissions.  Different
 *                   bag of bits than AArch64.
 *
 *   Long format   - 64-bit descriptors, up to 3 levels, MAIR table
 *                   for memory types, AttrIdx in the descriptor.
 *                   Essentially the same shape as AArch64 stage 1.
 *
 * For a learning kernel the long format is the better choice: the
 * descriptor bit layout, MAIR encoding, and walk structure are
 * almost the same as arch/aarch64/mmu.c, which makes the diff
 * between the two files mostly about which control register goes
 * where.  cortex-a15 / cortex-a7 (MT8125) both support LPAE.
 *
 * Address-space layout on QEMU `-M virt`
 * --------------------------------------
 *
 *     0x00000000  +-----------------------+
 *                 |  PL011 UART, GIC,     |  mapped Device-nGnRE
 *                 |  RTC, flash, ...      |   via L1[0] = 1 GB block
 *     0x40000000  +-----------------------+
 *                 |  RAM (>= 256 MB)      |  mapped Normal WBWA
 *                 |   kernel image, BSS,  |   via L1[1] = 1 GB block
 *                 |   page tables, heap   |
 *     0x80000000  +-----------------------+
 *                 |  unmapped             |  L1[2], L1[3] = invalid
 *     0xFFFFFFFF  +-----------------------+
 *
 * Page-table walk (LPAE, 4 KB granule, 32-bit VA)
 * -----------------------------------------------
 *
 *     31 30 29        21 20        12 11      0
 *    +--+--+-----------+-------------+--------+
 *    |L1|  |  L2 idx   |  L3 idx     | offset |
 *    +--+--+-----------+-------------+--------+
 *     |
 *     2-bit L1 index -> l1_table[0..3], each entry maps 1 GB
 *
 * The L1 has only 4 entries, so the L1 table itself is just 32 bytes
 * (and only needs 32-byte alignment).  We use 1 GB block descriptors
 * at L1 and don't need L2 tables at all yet.
 *
 * Descriptor bits (LPAE, same as AArch64 with one rename)
 * -------------------------------------------------------
 *   bit 0     valid
 *   bit 1     block (0) vs table (1)        -- at L1, L2
 *   bits 4:2  AttrIdx -- byte index into MAIR
 *   bit 5     NS (non-secure)
 *   bits 7:6  AP[2:1]  (00 = kernel RW, no user)
 *   bits 9:8  SH[1:0]  (11 = inner shareable for Normal,
 *                       00 = ignored for Device)
 *   bit 10    AF (Access Flag, must be 1)
 *   bit 11    nG
 *   bits 39:n output PA, where n depends on block size
 *   bit 53    PXN (privileged execute never)
 *   bit 54    XN  -- ARMv7 LPAE: unconditional Execute-Never for both
 *                    privilege levels.  This is the big trap-vs-AArch64
 *                    quirk: AArch64's bit 54 is UXN (user-only XN),
 *                    so setting it on kernel-RW pages is fine there.
 *                    On ARMv7, setting XN on the kernel block causes
 *                    the next instruction fetch after MMU enable to
 *                    take a prefetch abort (IFSR.status = 0x0d,
 *                    "permission fault level 1").  Kernel RAM must
 *                    therefore leave bit 54 clear.
 *
 * MAIR0 / MAIR1 -- the memory attribute table
 * -------------------------------------------
 * AArch64's MAIR_EL1 is a single 64-bit register holding 8 one-byte
 * attribute slots.  ARMv7 LPAE splits the same 8 bytes across two
 * 32-bit CP15 registers, MAIR0 (Attr0..Attr3) and MAIR1 (Attr4..Attr7).
 * We populate just two entries:
 *
 *   AttrIdx 0  Normal Outer+Inner WBWA       0xFF
 *   AttrIdx 2  Device-nGnRE                  0x04
 *
 *   MAIR0 = (0x04 << 16) | 0xFF = 0x000400FF
 *   MAIR1 = 0
 *
 * TTBCR -- Translation Table Base Control Register (LPAE form)
 * ------------------------------------------------------------
 *   bit 31    EAE = 1   (turn on LPAE = "use long format")
 *   bit 22    A1 = 0    (TTBR0 supplies the ASID)
 *   bit 13:12 SH0 = 11  (inner shareable table walks)
 *   bit 11:10 ORGN0 = 01 (outer WBWA)
 *   bit  9: 8 IRGN0 = 01 (inner WBWA)
 *   bit  5: 0 T0SZ = 0  (TTBR0 covers all 32 bits of VA)
 *
 *   Value: 0x80003500
 *
 * TTBR0 -- 64-bit base address register, written via MCRR pair
 * ------------------------------------------------------------
 *   bits 39:5  L1 table physical address  (low 5 bits = 0)
 *   bits 55:48 ASID
 *
 * SCTLR bits we flip on at the end (identical to AArch64):
 *   M  = 1  enable MMU
 *   C  = 1  enable D-cache
 *   I  = 1  enable I-cache
 */

#include <stdint.h>

#include "kappara/mmu.h"
#include "kappara/printk.h"

#define L1_ENTRIES		4

#define ATTR_NORMAL_IDX		0
#define ATTR_DEVICE_IDX		2

#define MAIR_NORMAL_WBWA	0xFFu
#define MAIR_DEVICE_NGNRE	0x04u
#define MAIR0_VALUE \
	(((uint32_t)MAIR_DEVICE_NGNRE << (ATTR_DEVICE_IDX * 8)) | \
	 ((uint32_t)MAIR_NORMAL_WBWA  << (ATTR_NORMAL_IDX * 8)))

#define TTBCR_EAE		(1u << 31)
#define TTBCR_SH0_INNER		(3u << 12)
#define TTBCR_ORGN0_WBWA	(1u << 10)
#define TTBCR_IRGN0_WBWA	(1u << 8)
#define TTBCR_T0SZ_FULL		(0u)

#define TTBCR_VALUE \
	(TTBCR_EAE | TTBCR_SH0_INNER | TTBCR_ORGN0_WBWA | \
	 TTBCR_IRGN0_WBWA | TTBCR_T0SZ_FULL)

#define D_VALID			(1ULL << 0)
#define D_TABLE			(1ULL << 1)
#define D_ATTRIDX(n)		(((uint64_t)(n) & 7) << 2)
#define D_AP_RW_EL1		(0ULL << 6)
#define D_SH_INNER		(3ULL << 8)
#define D_SH_NONE		(0ULL << 8)
#define D_AF			(1ULL << 10)
#define D_PXN			(1ULL << 53)
#define D_XN			(1ULL << 54)

#define SCTLR_M			(1u << 0)
#define SCTLR_C			(1u << 2)
#define SCTLR_I			(1u << 12)

/* L1 table: 4 entries, 32-byte aligned. */
__attribute__((aligned(32))) static uint64_t l1_table[L1_ENTRIES];

static void build_identity_map(void)
{
	/* L1[0]: 0x00000000..0x40000000  -> Device (UART, GIC, ...) */
	l1_table[0] = (uint64_t)0x00000000u |
		      D_VALID | D_ATTRIDX(ATTR_DEVICE_IDX) |
		      D_AP_RW_EL1 | D_SH_NONE | D_AF |
		      D_PXN | D_XN;

	/*
	 * L1[1]: 0x40000000..0x80000000  -> Normal cacheable (RAM).
	 *
	 * No XN: ARMv7 LPAE bit 54 (XN) is unconditional Execute-Never
	 * for BOTH privilege levels, unlike AArch64 where the same bit
	 * is UXN (user-only).  Setting it here would prefetch-abort the
	 * kernel on the next instruction after MMU enable.
	 */
	l1_table[1] = (uint64_t)0x40000000u |
		      D_VALID | D_ATTRIDX(ATTR_NORMAL_IDX) |
		      D_AP_RW_EL1 | D_SH_INNER | D_AF;

	l1_table[2] = 0;
	l1_table[3] = 0;
}

void mmu_init(void)
{
	build_identity_map();

	uint32_t mair0  = MAIR0_VALUE;
	uint32_t mair1  = 0;
	uint32_t ttbcr  = TTBCR_VALUE;
	uint32_t ttbr_l = (uint32_t)(uintptr_t)l1_table;
	uint32_t ttbr_h = 0;

	__asm__ volatile (
		"mcr	p15, 0, %0, c10, c2, 0\n"	/* MAIR0   */
		"mcr	p15, 0, %1, c10, c2, 1\n"	/* MAIR1   */
		"mcr	p15, 0, %2, c2,  c0, 2\n"	/* TTBCR   */
		"mcrr	p15, 0, %3, %4,  c2\n"		/* TTBR0   */
		"isb\n"
		"mcr	p15, 0, %5, c8,  c7, 0\n"	/* TLBIALL */
		"dsb\n"
		"isb\n"
		:
		: "r"(mair0), "r"(mair1), "r"(ttbcr),
		  "r"(ttbr_l), "r"(ttbr_h), "r"(0u)
		: "memory");

	uint32_t sctlr;
	__asm__ volatile ("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
	sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;

	__asm__ volatile (
		"mcr	p15, 0, %0, c7, c5, 0\n"	/* ICIALLU */
		"dsb\n"
		"mcr	p15, 0, %1, c1, c0, 0\n"	/* SCTLR   */
		"isb\n"
		:
		: "r"(0u), "r"(sctlr)
		: "memory");

	kprintf("mmu: enabled (ttbr0=0x%lx ttbcr=0x%lx mair0=0x%lx)\n",
		(unsigned long)ttbr_l,
		(unsigned long)ttbcr,
		(unsigned long)mair0);
}

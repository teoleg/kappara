#include <stdint.h>

#include "kappara/mmu.h"
#include "kappara/pmm.h"
#include "kappara/printk.h"

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
#define D_SH_INNER		(3UL << 8)
#define D_SH_NONE		(0UL << 8)
#define D_AF			(1UL << 10)
#define D_PXN			(1UL << 53)
#define D_UXN			(1UL << 54)

/* 2 MB block at L2. */
#define BLOCK_2M_SHIFT		21
#define BLOCK_2M_SIZE		(1UL << BLOCK_2M_SHIFT)

#define PERIPH_BASE		0x3F000000UL
#define PERIPH_END		0x40000000UL

/* SCTLR_EL1 bits we flip on. */
#define SCTLR_M			(1UL << 0)
#define SCTLR_C			(1UL << 2)
#define SCTLR_I			(1UL << 12)

__attribute__((aligned(PAGE_SIZE))) static uint64_t l0_table[ENTRIES_PER_TABLE];
__attribute__((aligned(PAGE_SIZE))) static uint64_t l1_table[ENTRIES_PER_TABLE];
__attribute__((aligned(PAGE_SIZE))) static uint64_t l2_table[ENTRIES_PER_TABLE];

static void build_identity_map(void)
{
	l0_table[0] = (uint64_t)(uintptr_t)l1_table | D_VALID | D_TABLE;
	l1_table[0] = (uint64_t)(uintptr_t)l2_table | D_VALID | D_TABLE;

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

void mmu_init(void)
{
	build_identity_map();

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

	kprintf("mmu: enabled (ttbr0=0x%lx tcr=0x%lx mair=0x%lx)\n",
		(unsigned long)ttbr0,
		(unsigned long)TCR_VALUE,
		(unsigned long)MAIR_VALUE);
}

/*
 * uts/virt/gic.c -- GIC v3 distributor + redistributor + sysreg CPU iface
 *
 * Cortex-A53/A72 are GIC v3-capable and reset with the v3 system
 * register interface live (ICC_SRE_EL1.SRE=1).  QEMU virt with the
 * default gic-version=3 emulates exactly that.  v3's CPU interface
 * isn't MMIO at all -- IRQ ack/EOI go through ICC_IAR1_EL1 /
 * ICC_EOIR1_EL1 sysregs.  The only MMIO is the distributor and the
 * per-CPU redistributor.
 *
 * Memory map (QEMU virt aarch64 default):
 *   Distributor       0x08000000 + 0x10000   (shared)
 *   Redistributor 0   0x080A0000 + 0x20000   (per-CPU, 0x20000 stride)
 *   Redistributor 1   0x080C0000 + 0x20000
 *     ...
 *
 * Redistributor layout per CPU (0x20000 = LPI bank 0x10000 + SGI bank 0x10000):
 *   RD bank (offset 0):
 *     GICR_CTLR    0x000
 *     GICR_TYPER   0x008
 *     GICR_WAKER   0x014
 *   SGI bank (offset 0x10000):
 *     GICR_IGROUPR0    0x080  (SGIs 0-15 + PPIs 16-31)
 *     GICR_ISENABLER0  0x100
 *     GICR_ICENABLER0  0x180
 *     GICR_ISPENDR0    0x200
 *     GICR_IPRIORITYR0 0x400  (per-intid 8-bit priorities)
 *     GICR_ICFGR0      0xC00  (SGIs, RO)
 *     GICR_ICFGR1      0xC04  (PPIs, RW)
 *
 * Bring-up sequence (RFC 6: ARM IHI 0069F section 7.6):
 *   1. ICC_SRE_EL1.SRE = 1  (use sysreg interface)
 *   2. GICD_CTLR: ARE_NS + EnableGrp1NS
 *   3. GICR_WAKER: clear ProcessorSleep, wait ChildrenAsleep=0
 *   4. Configure per-PPI: IGROUPR / IPRIORITYR / ISENABLER
 *   5. ICC_PMR_EL1 = 0xff, ICC_BPR1_EL1 = 0
 *   6. ICC_IGRPEN1_EL1 = 1
 */

#include <stdint.h>

#include "kappara/arch/acpi.h"
#include "kappara/arch/gic.h"
#include "kappara/arch/mmu.h"
#include "kappara/core/printk.h"
#include "platform.h"

/* Distributor offsets */
#define GICD_CTLR		0x000
#define GICD_IGROUPR(n)		(0x080 + 4 * (n))
#define GICD_ISENABLER(n)	(0x100 + 4 * (n))
#define GICD_ICENABLER(n)	(0x180 + 4 * (n))
#define GICD_IPRIORITYR(n)	(0x400 + 4 * (n))
#define GICD_IROUTER(n)		(0x6000 + 8 * (n))	/* per-SPI 64-bit */

#define GICD_CTLR_ARE_NS	(1u << 4)
#define GICD_CTLR_ENGRP1_NS	(1u << 1)

/* Redistributor RD bank offsets */
#define GICR_CTLR		0x000
#define GICR_WAKER		0x014
#define GICR_WAKER_PS		(1u << 1)	/* ProcessorSleep */
#define GICR_WAKER_CA		(1u << 2)	/* ChildrenAsleep */

/* Redistributor SGI bank offsets (within RD + 0x10000) */
#define GICR_IGROUPR0		(0x10000 + 0x080)
#define GICR_ISENABLER0		(0x10000 + 0x100)
#define GICR_ICENABLER0		(0x10000 + 0x180)
#define GICR_IPRIORITYR(n)	(0x10000 + 0x400 + 4 * (n))
#define GICR_ICFGR1		(0x10000 + 0xC04)

/* Runtime GIC base addresses.  Set by gic_dist_init() from ACPI on
 * UEFI boot (AWS Graviton); fall back to compile-time platform
 * constants under -kernel dev boot (QEMU virt). */
static uintptr_t gic_dist_base;
static uintptr_t gic_redist_base;
static int       gic_dist_inited;

static inline void d_write(unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(gic_dist_base + off) = v;
}
static inline uint32_t d_read(unsigned off)
{
	return *(volatile uint32_t *)(gic_dist_base + off);
}
static inline uintptr_t redist_base(unsigned cpu)
{
	return gic_redist_base + (uintptr_t)cpu * PLAT_GIC_REDIST_STRIDE;
}
static inline void r_write(unsigned cpu, unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(redist_base(cpu) + off) = v;
}
static inline uint32_t r_read(unsigned cpu, unsigned off)
{
	return *(volatile uint32_t *)(redist_base(cpu) + off);
}

static inline unsigned this_cpu_id(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, mpidr_el1" : "=r"(v));
	return (unsigned)(v & 0xff);
}

void gic_dist_init(void)
{
	if (gic_dist_inited)
		return;
	gic_dist_inited = 1;

	if (acpi_present && acpi_gicd_base) {
		gic_dist_base   = (uintptr_t)acpi_gicd_base;
		gic_redist_base = (uintptr_t)acpi_gicr_base;
	} else {
		gic_dist_base   = PLAT_GIC_DIST_BASE;
		gic_redist_base = PLAT_GIC_REDIST_BASE;
	}

	kprintf("gic: dist_init base=0x%lx redist=0x%lx\n",
		(unsigned long)gic_dist_base, (unsigned long)gic_redist_base);

	/* Map GIC MMIO if the ACPI-reported address falls above the static
	 * 2 GB identity map boundary.  Only call for addresses >= 0x80000000:
	 * L1[0] (0..1 GB via L2 table) and L1[1] (0x40000000..0x7FFFFFFF
	 * Normal RAM block) are already wired and must not be overwritten. */
	if (gic_dist_base >= 0x80000000UL)
		mmu_map_device_1gb(gic_dist_base);
	if (gic_redist_base >= 0x80000000UL)
		mmu_map_device_1gb(gic_redist_base);

	if (acpi_present) {
		/* UEFI already fully configured the GIC distributor:
		 * ARE_NS=1, EnableGrp1NS=1, groups and priorities set.
		 * On Nitro/Graviton any write to GICD_CTLR from non-secure
		 * EL1 -- even setting ARE_NS=1 when it's already 1 -- is
		 * treated as a security violation by the hypervisor and
		 * causes a silent guest reset (no EL1 trap, no crash dump,
		 * just watchdog reboot).  Same applies to the SPI
		 * ICENABLER/IGROUPR sweep: UEFI already set these correctly,
		 * and re-writing them from NS EL1 triggers the same trap.
		 * Trust UEFI's config; gic_enable_irq() handles individual
		 * enables when the driver needs them. */
		return;
	}

	/* QEMU -kernel path: GIC is in reset state, configure from scratch. */

	/* Disable all SPIs and put them in Group 1 NS.  ISENABLER 0 is
	 * banked per-CPU (covers SGIs+PPIs); SPIs start at n=1. */
	uint32_t typer = d_read(0x004);
	unsigned itlines = (typer & 0x1f) + 1;	/* number of 32-int blocks */
	for (unsigned n = 1; n < itlines; n++) {
		d_write(GICD_ICENABLER(n), 0xffffffffu);
		d_write(GICD_IGROUPR(n),   0xffffffffu);
	}
	for (unsigned n = 0; n < itlines * 8; n++)
		d_write(GICD_IPRIORITYR(n), 0xa0a0a0a0u);

	/* ARE_NS + EnableGrp1NS.  SPIs routed to CPU 0 via IROUTER=0. */
	d_write(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENGRP1_NS);
}

void gic_cpu_init(void)
{
	unsigned cpu = this_cpu_id();

	kprintf("gic: cpu_init cpu=%u redist_base=0x%lx\n",
		cpu, (unsigned long)(gic_redist_base + (uintptr_t)cpu * PLAT_GIC_REDIST_STRIDE));

	/* Wake the per-CPU redistributor: clear ProcessorSleep, then
	 * spin until the GIC clears ChildrenAsleep. */
	uint32_t waker = r_read(cpu, GICR_WAKER);
	kprintf("gic: GICR_WAKER initial=0x%x\n", waker);
	waker &= ~GICR_WAKER_PS;
	r_write(cpu, GICR_WAKER, waker);
	{
		uint64_t freq, start, deadline;
		__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
		__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
		deadline = start + freq;	/* 1 s */
		while (r_read(cpu, GICR_WAKER) & GICR_WAKER_CA) {
			uint64_t now;
			__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
			if (now >= deadline) {
				kprintf("gic: GICR_WAKER ChildrenAsleep stuck "
					"(WAKER=0x%x) -- bad redistributor addr?\n",
					r_read(cpu, GICR_WAKER));
				kpanic("GIC redistributor not responding");
			}
		}
	}
	kprintf("gic: GICR_WAKER woken\n");

	/* Per-CPU SGI/PPI config in the redistributor SGI bank: all in
	 * Group 1, priority 0xa0, enable nothing yet (per-IRQ enable
	 * comes via gic_enable_irq). */
	r_write(cpu, GICR_IGROUPR0,   0xffffffffu);
	r_write(cpu, GICR_ICENABLER0, 0xffffffffu);
	for (unsigned n = 0; n < 8; n++)
		r_write(cpu, GICR_IPRIORITYR(n), 0xa0a0a0a0u);
	/* ICFGR1: PPIs (intids 16..31) are 2 bits each, 0=level, 2=edge.
	 * Architectural timers are level-sensitive. */
	r_write(cpu, GICR_ICFGR1, 0);

	/* Enable the v3 sysreg CPU interface.  ICC_SRE_EL1.SRE = 1
	 * lets us use ICC_IAR1_EL1/ICC_EOIR1_EL1; QEMU virt with EL2
	 * has ICC_SRE_EL2.Enable set by default so EL1 access is
	 * permitted. */
	uint64_t sre = 1;	/* SRE = 1 */
	__asm__ volatile ("msr S3_0_C12_C12_5, %0" :: "r"(sre));	/* ICC_SRE_EL1 */
	__asm__ volatile ("isb");

	/* PMR = 0xff (all priorities allowed).  BPR1 = 0. */
	uint64_t v = 0xff;
	__asm__ volatile ("msr S3_0_C4_C6_0, %0" :: "r"(v));	/* ICC_PMR_EL1 */
	v = 0;
	__asm__ volatile ("msr S3_0_C12_C12_3, %0" :: "r"(v));	/* ICC_BPR1_EL1 */
	/* IGRPEN1 = 1. */
	v = 1;
	__asm__ volatile ("msr S3_0_C12_C12_7, %0" :: "r"(v));	/* ICC_IGRPEN1_EL1 */
	__asm__ volatile ("isb");

}

void gic_enable_irq(unsigned intid)
{
	if (intid < 32) {
		/* SGI / PPI: per-CPU redistributor. */
		unsigned cpu = this_cpu_id();
		r_write(cpu, GICR_ISENABLER0, 1u << intid);
		r_write(cpu, GICR_IPRIORITYR(intid / 4), 0xa0a0a0a0u);
	} else {
		/* SPI: distributor. */
		d_write(GICD_ISENABLER(intid / 32), 1u << (intid % 32));
	}
}

uint32_t gic_iar(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, S3_0_C12_C12_0" : "=r"(v));	/* ICC_IAR1_EL1 */
	return (uint32_t)v;
}

void gic_eoi(uint32_t iar)
{
	uint64_t v = iar;
	__asm__ volatile ("msr S3_0_C12_C12_1, %0" :: "r"(v));	/* ICC_EOIR1_EL1 */
}

void gic_send_sgi(unsigned target_cpu, unsigned sgi)
{
	/* ICC_SGI1R_EL1 layout for targeting a single CPU's aff0:
	 *   bits 23:16  TargetList (1 << aff0)
	 *   bits  3:0   INTID
	 * Other affinity / cluster bits = 0 for single-cluster systems. */
	uint64_t v = ((uint64_t)(1u << (target_cpu & 0xf)) << 0)
	           | ((uint64_t)(sgi & 0xf) << 24);
	__asm__ volatile ("msr S3_0_C12_C11_5, %0" :: "r"(v));	/* ICC_SGI1R_EL1 */
	__asm__ volatile ("isb");
}

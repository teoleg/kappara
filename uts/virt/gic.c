/*
 * uts/virt/gic.c -- GIC v2 distributor + CPU-interface driver
 *
 * The virt machine has none of the BCM2836 local-peripheral block that
 * Pi 3 uses; interrupts are routed through ARM's GIC v2 instead.
 * This is the IRQ controller you'll see on basically every aarch64
 * SoC that's not from Broadcom (and on every cloud VM that uses
 * `-machine virt`).
 *
 * Programming model (GIC v2; ARM DDI 0471B):
 *
 *   Distributor (0x08000000)
 *     GICD_CTLR  (0x000)  bit 0 = enable distributor
 *     GICD_ISENABLER<N>   bit (intid % 32) = enable interrupt
 *     GICD_IPRIORITYR<N>  8-bit priority per intid (0 = highest)
 *     GICD_ITARGETSR<N>   per-SPI target-CPU bitmap (SPIs only)
 *     GICD_SGIR  (0xF00)  trigger SGI to target CPUs
 *
 *   CPU interface (0x08010000)
 *     GICC_CTLR  (0x000)  bit 0 = enable CPU interface
 *     GICC_PMR   (0x004)  priority mask (must be > 0 to take any IRQ)
 *     GICC_IAR   (0x00C)  read returns the active INTID
 *     GICC_EOIR  (0x010)  write INTID to mark interrupt complete
 *
 * Interrupt classes (ARMv8 GIC):
 *   SGI  intid  0..15   software-generated (used for IPIs)
 *   PPI  intid 16..31   per-CPU peripheral (generic timer is PPI 30)
 *   SPI  intid 32+      shared peripheral (virtio devices land here)
 *
 * Single-CPU first cut: distributor enable, CPU 0's interface enable,
 * priority mask wide open.  Per-interrupt enable happens from
 * timer_init() / virtio_probe() etc., not here.
 */

#include <stdint.h>

#include "kappara/arch/gic.h"
#include "platform.h"

#define GICD_CTLR		0x000
#define GICD_IGROUPR(n)		(0x080 + 4 * (n))
#define GICD_ISENABLER(n)	(0x100 + 4 * (n))
#define GICD_ICENABLER(n)	(0x180 + 4 * (n))
#define GICD_ISPENDR(n)		(0x200 + 4 * (n))
#define GICD_IPRIORITYR(n)	(0x400 + 4 * (n))
#define GICD_ITARGETSR(n)	(0x800 + 4 * (n))
#define GICD_ICFGR(n)		(0xC00 + 4 * (n))
#define GICD_SGIR		0xF00

#define GICC_CTLR		0x000
#define GICC_PMR		0x004
#define GICC_BPR		0x008
#define GICC_IAR		0x00C
#define GICC_EOIR		0x010

static inline void d_write(unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(PLAT_GIC_DIST_BASE + off) = v;
}
static inline uint32_t d_read(unsigned off)
{
	return *(volatile uint32_t *)(PLAT_GIC_DIST_BASE + off);
}
static inline void c_write(unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(PLAT_GIC_CPUI_BASE + off) = v;
}
static inline uint32_t c_read(unsigned off)
{
	return *(volatile uint32_t *)(PLAT_GIC_CPUI_BASE + off);
}

void gic_dist_init(void)
{
	/* Disable distributor while we configure it. */
	d_write(GICD_CTLR, 0);

	/* Mask everything: clear-enable for every interrupt. */
	for (unsigned n = 0; n < 32; n++)
		d_write(GICD_ICENABLER(n), 0xffffffffu);

	/* All interrupts go in Group 1 (non-secure).  The non-secure
	 * view of GICD_CTLR only routes Group 1; without this, an EL1
	 * non-secure CPU would never see anything come through.  We're
	 * not using secure-side handlers, so blanket-assigning all
	 * intids to group 1 is the cleanest setup. */
	for (unsigned n = 0; n < 32; n++)
		d_write(GICD_IGROUPR(n), 0xffffffffu);

	/* Default priority 0x80 for every interrupt -- mid-range,
	 * matches Linux's default for GIC v2 without security ext. */
	for (unsigned n = 0; n < 256; n++)
		d_write(GICD_IPRIORITYR(n), 0x80808080);

	/* Target every SPI at CPU 0 (only CPU we're using right now).
	 * ITARGETSR 0..7 are read-only PPIs; start at 8. */
	for (unsigned n = 8; n < 64; n++)
		d_write(GICD_ITARGETSR(n), 0x01010101u);

	/* Enable Group 1 (non-secure) interrupts at distributor. */
	d_write(GICD_CTLR, 1);
}

void gic_cpu_init(void)
{
	/* Allow every priority to interrupt us (0xff = lowest mask,
	 * priorities 0..0xfe are above it).  BPR=0 means no
	 * preemption-group bits. */
	c_write(GICC_PMR, 0xff);
	c_write(GICC_BPR, 0);
	c_write(GICC_CTLR, 1);
}

void gic_enable_irq(unsigned intid)
{
	d_write(GICD_ISENABLER(intid / 32), 1u << (intid % 32));
}

uint32_t gic_iar(void)
{
	return c_read(GICC_IAR);
}

void gic_eoi(uint32_t iar)
{
	c_write(GICC_EOIR, iar);
}

void gic_send_sgi(unsigned target_cpu, unsigned sgi)
{
	/* GICD_SGIR fields:
	 *   bits 26:24  TargetListFilter (00 = use bits 23:16)
	 *   bits 23:16  CPUTargetList    (one bit per CPU)
	 *   bits 15:4   reserved
	 *   bits  3:0   SGI INTID (0..15) */
	uint32_t v = ((1u << (target_cpu & 0x7)) << 16) | (sgi & 0xf);
	d_write(GICD_SGIR, v);
	__asm__ volatile ("dsb sy" ::: "memory");
}

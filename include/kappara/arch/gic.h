/*
 * include/kappara/arch/gic.h -- GIC v2 driver API (virt target)
 *
 * Lives behind ARCH=virt; raspi3b uses the BCM2836 local-peripheral
 * block instead and never includes this.  Anyone touching IRQs on
 * virt -- the timer, the IPI backend, virtio probe -- goes through
 * here so the GIC programming details don't leak.
 */

#ifndef KAPPARA_ARCH_GIC_H
#define KAPPARA_ARCH_GIC_H

#include <stdint.h>

/* One-time distributor setup.  Call from kmain on the boot CPU before
 * any interrupt source is configured. */
void     gic_dist_init(void);

/* Per-CPU interface setup.  Call from each core's bring-up path
 * before it unmasks DAIF. */
void     gic_cpu_init(void);

/* Mark an interrupt ID as enabled in the distributor.  Idempotent. */
void     gic_enable_irq(unsigned intid);

/* Acknowledge the active IRQ and return its INTID.  Pair with
 * gic_eoi() once the handler is done. */
uint32_t gic_iar(void);
void     gic_eoi(uint32_t iar);

/* Trigger an SGI (software-generated interrupt) on `target_cpu`. */
void     gic_send_sgi(unsigned target_cpu, unsigned sgi);

#endif

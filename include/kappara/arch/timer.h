/*
 * include/kappara/timer.h -- timer + IRQ dispatch on AArch64 Pi 3
 *
 * timer_init(hz) arms the ARMv8 generic timer at hz ticks/sec and
 * routes its IRQ to core 0 via the BCM2836 ARM-local controller.
 *
 * irq_dispatch() is called from trap_dispatch (arch/aarch64/trap.c)
 * for every IRQ vector entry.  It reads the BCM2836 IRQ_SOURCE
 * register and demultiplexes to per-device handlers.  Today there's
 * only one source (the timer); more will join as drivers land.
 */
#ifndef KAPPARA_TIMER_H
#define KAPPARA_TIMER_H

void timer_init(unsigned hz);
void timer_init_this_cpu(void);
void irq_dispatch(void);

#endif

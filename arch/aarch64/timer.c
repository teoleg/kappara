#include <stdint.h>

#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/timer.h"

/*
 * BCM2836/2837 "ARM local" peripherals, used by the Pi 3 to route per-core
 * timer interrupts before the generic timer hits a CPU's IRQ line.
 */
#define LOCAL_BASE		0x40000000UL
#define CORE0_TIMER_CTL		(LOCAL_BASE + 0x40)
#define CORE0_IRQ_SRC		(LOCAL_BASE + 0x60)

/* TIMER_CONTROL bits: [3:0] route CNTP{S,NS,HP,V}IRQ to IRQ, [7:4] to FIQ. */
#define ROUTE_CNTPNSIRQ_IRQ	(1u << 1)

/* IRQ_SRC bits: bit 1 = CNTPNSIRQ pending. */
#define IRQ_CNTPNSIRQ		(1u << 1)

static uint64_t tick_cycles;

static inline uint64_t read_cntfrq(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static inline void write_cntp_tval(uint64_t v)
{
	__asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void write_cntp_ctl(uint64_t v)
{
	__asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(v));
}

void timer_init(unsigned hz)
{
	uint64_t freq = read_cntfrq();
	tick_cycles = freq / hz;

	*(volatile uint32_t *)CORE0_TIMER_CTL = ROUTE_CNTPNSIRQ_IRQ;

	write_cntp_tval(tick_cycles);
	write_cntp_ctl(1);	/* ENABLE=1, IMASK=0 */

	kprintf("timer: cntfrq=%lu Hz, tick=%u Hz (%lu cycles)\n",
		(unsigned long)freq, hz, (unsigned long)tick_cycles);
}

static void timer_irq(void)
{
	write_cntp_tval(tick_cycles);
	sched_tick();
}

void irq_dispatch(void)
{
	uint32_t src = *(volatile uint32_t *)CORE0_IRQ_SRC;

	if (src & IRQ_CNTPNSIRQ) {
		timer_irq();
		return;
	}

	kprintf("irq: spurious (src=0x%x)\n", src);
}

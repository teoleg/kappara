/*
 * include/kappara/ipi.h -- inter-processor interrupts (BCM2836 mailbox)
 *
 * What this is
 * ------------
 * Four cores, four mailboxes per core, four pending bits in each
 * core's local IRQ_SOURCE.  Writing to a peer's mailbox-0-set
 * register raises an IRQ on that peer (if the peer enabled mailbox 0
 * in its MBOX_IRQ_CTL).  That's all an IPI is here: a kick to wake
 * the target from WFI so it re-enters the dispatcher.
 *
 * We don't (yet) encode a reason in mailbox payload bits.  The IPI
 * IS the reason: "look at your runqueue, somebody might have parked
 * work on it, or you should re-check whether you can steal."  The
 * scheduler already does the right thing on return from idle.
 *
 * Pi 4 / GIC port note
 * --------------------
 * The Pi 4 has a real GICv2; IPIs there are SGIs (software-generated
 * interrupts) instead of mailbox writes.  The API (ipi_send /
 * ipi_handle / ipi_init_this_cpu) is shaped to be portable so the
 * BCM2836 backend can be swapped for a GIC backend without touching
 * the scheduler.
 */
#ifndef KAPPARA_IPI_H
#define KAPPARA_IPI_H

/* Enable mailbox-0 IRQ on the calling CPU.  Each secondary calls this
 * from secondary_main before unmasking IRQs; core 0 calls it from
 * kmain. */
void ipi_init_this_cpu(void);

/* Send a wake-up IPI to `target_cpu`.  No-op if `target_cpu` is
 * out of range or equals the calling CPU. */
void ipi_send(unsigned target_cpu);

/* Send wake-up IPIs to every CPU currently advertised as idle
 * (sched.c maintains a bitmask).  Pure best-effort: a CPU that
 * woke up between the read and the write just gets a redundant
 * IRQ, which is harmless. */
void ipi_wake_idle(void);

/* Called from irq_dispatch when the local IRQ_SOURCE shows mailbox 0
 * pending.  Reads + clears the mailbox; the act of returning from
 * the IRQ kicks the dispatcher into another scheduling round. */
void ipi_handle(void);

#endif

/*
 * arch/arm/trap.c -- ARMv7-A trap dispatch and fault dump
 * =======================================================
 *
 * What this file is
 * -----------------
 * The C side of the exception path that arch/arm/vectors.S funnels
 * everything into.  trap_init() programs the Vector Base Address
 * Register (VBAR) so the CPU finds arm_vectors when something faults,
 * and trap_dump() turns the saved frame into a human-readable crash
 * report and panics.
 *
 * For now every exception is a panic.  Once we add IRQ handling we
 * will split this into an IRQ path (which acks the controller, runs
 * the device handler, and returns via KERNEL_EXIT) and a sync-fault
 * path (which still dumps and panics for now).
 *
 * Trap frame contract with vectors.S
 * ----------------------------------
 *
 *     +---------------------+
 *     |  spsr   (uint32_t)  |   +60     CPSR at trap time
 *     +---------------------+
 *     |  pc     (uint32_t)  |   +56     return PC (LR_<m>, adjusted)
 *     +---------------------+
 *     |  lr     (uint32_t)  |   +52     LR_svc at save time
 *     +---------------------+
 *     |  r12 .. r0          |   +0..+48
 *     +---------------------+ <-- struct arm_trap_frame*
 *
 * VBAR (Vector Base Address Register)
 * -----------------------------------
 * Programmed via MCR p15, 0, <Rt>, c12, c0, 0.  The CPU adds the
 * vector offset (0x00 .. 0x1C) to this and jumps there.  The base
 * must be aligned (low 5 bits read as zero on cortex-a15), and the
 * table itself must fit in a single page; arm_vectors is .balign 32
 * inside the kernel image so both are satisfied trivially.
 *
 * CPSR.M decoding
 * ---------------
 * The processor mode at the time of the trap is in SPSR[4:0]:
 *
 *     0x10  USR    -- user mode (we will use this for processes)
 *     0x11  FIQ    -- fast interrupt
 *     0x12  IRQ    -- interrupt
 *     0x13  SVC    -- supervisor (kernel runs here normally)
 *     0x17  ABT    -- abort (entered on PAbt/DAbt before our cps)
 *     0x1A  HYP    -- hypervisor (we leave this after boot.S)
 *     0x1B  UND    -- undefined-instruction handler mode
 *     0x1F  SYS    -- system (shares USR registers, privileged)
 */

#include <stdint.h>

#include "kappara/printk.h"
#include "kappara/syscall.h"
#include "kappara/timer.h"
#include "kappara/trap.h"
#include "kappara/uaccess.h"

struct arm_trap_frame {
	uint32_t r[13];		/* r0..r12 */
	uint32_t lr;		/* LR_svc at save time */
	uint32_t pc;		/* return PC of the interrupted code */
	uint32_t spsr;		/* SPSR_<entry_mode> = CPSR at trap time */
};

extern char arm_vectors[];

void trap_init(void)
{
	__asm__ volatile (
		"mcr	p15, 0, %0, c12, c0, 0\n"	/* VBAR <- arm_vectors */
		"isb\n"
		:: "r"(arm_vectors)
		: "memory");
}

static const char *vec_name(unsigned v)
{
	static const char *const t[] = {
		"reset", "undef",    "svc", "pabt",
		"dabt",  "reserved", "irq", "fiq",
	};
	return v < 8 ? t[v] : "?";
}

static const char *mode_name(unsigned m)
{
	switch (m & 0x1f) {
	case 0x10: return "USR";
	case 0x11: return "FIQ";
	case 0x12: return "IRQ";
	case 0x13: return "SVC";
	case 0x17: return "ABT";
	case 0x1a: return "HYP";
	case 0x1b: return "UND";
	case 0x1f: return "SYS";
	default:   return "??";
	}
}

void trap_dispatch(struct arm_trap_frame *tf, unsigned vec_id);
void trap_dispatch(struct arm_trap_frame *tf, unsigned vec_id)
{
	/* IRQ vector returns; SVC routes to the syscall layer; everything
	 * else dumps and panics. */
	if (vec_id == 6) {
		irq_dispatch();
		return;
	}

	/* SVC (vec_id == 2): r7 holds the syscall number, r0..r5 the args.
	 * The frame's saved PC already points at the instruction after
	 * `svc #imm` (we passed lr_adj=0 to VEC_HANDLER for SVC), so the
	 * result we stash in tf->r[0] becomes the value the user sees.
	 *
	 * cpsie i unmasks IRQs so syscalls are preemptible -- same
	 * reasoning as the AArch64 daifclr in arch/aarch64/trap.c. */
	if (vec_id == 2) {
		/* CPSR.M[4:0] == 0x10 means the caller was USR mode (EL0
		 * equivalent on ARMv7).  ARMv7 has no EL0 userspace yet,
		 * so today this stays 0 -- but the wiring is here for when
		 * it does. */
		syscall_from_user = ((tf->spsr & 0x1f) == 0x10) ? 1 : 0;
		__asm__ volatile ("cpsie i" ::: "memory");
		tf->r[0] = (uint32_t)syscall_dispatch(
				(long)tf->r[7],
				(long)tf->r[0], (long)tf->r[1], (long)tf->r[2],
				(long)tf->r[3], (long)tf->r[4], (long)tf->r[5]);
		syscall_from_user = 0;
		return;
	}

	kprintf("\n!! trap: %s (vec=%u)\n", vec_name(vec_id), vec_id);
	kprintf("   pc=0x%08lx  spsr=0x%08lx (pre-mode=%s)\n",
		(unsigned long)tf->pc,
		(unsigned long)tf->spsr,
		mode_name(tf->spsr));
	for (int i = 0; i < 12; i += 2) {
		kprintf("   r%-2d=0x%08lx  r%-2d=0x%08lx\n",
			i,     (unsigned long)tf->r[i],
			i + 1, (unsigned long)tf->r[i + 1]);
	}
	kprintf("   r12=0x%08lx  lr=0x%08lx\n",
		(unsigned long)tf->r[12],
		(unsigned long)tf->lr);

	kpanic("unhandled trap");
}

/*
 * arch/aarch64/trap.c -- AArch64 exception dispatch
 * =================================================
 *
 * What this file is
 * -----------------
 * The C side of the exception path.  arch/aarch64/vectors.S has just
 * saved a 288-byte struct trap_frame on the kernel stack and called
 * trap_dispatch(tf, vec_id).  This file decides whether it's an IRQ
 * (route to irq_dispatch in the timer driver) or a synchronous fault
 * (decode ESR_EL1, dump every register, panic).
 *
 * ESR_EL1 layout (the Exception Syndrome Register)
 * -----------------------------------------------
 *
 *      bit 31         26 25 24                 0
 *     +-----------------+--+---------------------+
 *     |       EC        |IL|         ISS         |
 *     +-----------------+--+---------------------+
 *      Exception class   |  Instruction-Specific
 *                        Length bit (1=32b instr)
 *
 *   EC encodes the cause of the synchronous exception.  ec_name() has
 *   the cases we'll see during bring-up; full list is in the ARM ARM
 *   table D17-2 (~70 entries).  Most useful for us:
 *
 *     0x21  Instruction Abort from current EL  -- bad PC translation
 *     0x25  Data Abort       from current EL  -- bad load/store
 *     0x22  PC alignment fault
 *     0x26  SP alignment fault
 *     0x2F  SError
 *     0x3C  brk #imm  (AArch64 software breakpoint)
 *
 * trap_init
 * ---------
 *   msr VBAR_EL1, <addr of arm_vectors>
 *   isb
 * After this, any exception while at EL1 follows the table.  EL1h is
 * the SP-banked variant; we use it (the boot.S SPSR_EL2 = 0x3C5
 * sets M[4:0]=0101 = EL1h), so synchronous faults / IRQs / FIQs /
 * SErrors all land in the 0x200..0x380 quarter of the table.
 */

#include <stdint.h>

#include "kappara/kallsyms.h"
#include "kappara/printk.h"
#include "kappara/sched.h"	/* cur */
#include "kappara/signal.h"
#include "kappara/syscall.h"
#include "kappara/timer.h"
#include "kappara/trap.h"
#include "kappara/uaccess.h"
#include "kappara/user.h"	/* sys_exit_impl */

extern char vectors[];

void trap_init(void)
{
	__asm__ volatile (
		"msr	vbar_el1, %0\n"
		"isb\n"
		: : "r"(vectors) : "memory");
}

static const char *vec_name(unsigned id)
{
	static const char *const names[] = {
		"sync_sp0",   "irq_sp0",   "fiq_sp0",   "serror_sp0",
		"sync_spx",   "irq_spx",   "fiq_spx",   "serror_spx",
		"sync_lo64",  "irq_lo64",  "fiq_lo64",  "serror_lo64",
		"sync_lo32",  "irq_lo32",  "fiq_lo32",  "serror_lo32",
	};
	return id < 16 ? names[id] : "??";
}

static const char *ec_name(unsigned ec)
{
	switch (ec) {
	case 0x00: return "unknown";
	case 0x07: return "SIMD/FP trap";
	case 0x15: return "svc (aarch64)";
	case 0x18: return "msr/mrs/sys trap";
	case 0x20: return "instruction abort (lower EL)";
	case 0x21: return "instruction abort (curr EL)";
	case 0x22: return "PC alignment";
	case 0x24: return "data abort (lower EL)";
	case 0x25: return "data abort (curr EL)";
	case 0x26: return "SP alignment";
	case 0x2c: return "FP exception";
	case 0x2f: return "SError";
	case 0x30: return "breakpoint (lower EL)";
	case 0x31: return "breakpoint (curr EL)";
	case 0x32: return "software step (lower EL)";
	case 0x33: return "software step (curr EL)";
	case 0x34: return "watchpoint (lower EL)";
	case 0x35: return "watchpoint (curr EL)";
	case 0x3c: return "brk (aarch64)";
	default:   return "(other)";
	}
}

void trap_dispatch(struct trap_frame *tf, unsigned vec_id)
{
	if (vec_id == VEC_IRQ_SP0 || vec_id == VEC_IRQ_SPX ||
	    vec_id == VEC_IRQ_LO64 || vec_id == VEC_IRQ_LO32) {
		irq_dispatch();
		return;
	}

	unsigned ec  = (unsigned)((tf->esr >> 26) & 0x3f);
	unsigned iss = (unsigned)(tf->esr & 0x1ffffff);

	/*
	 * SVC from AArch64.  ELR_EL1 already points at the instruction
	 * AFTER the svc, so no PC adjustment is needed; the result we
	 * stash in tf->x[0] becomes the return value the user sees in x0.
	 *
	 * Unmask IRQs (PSTATE.I=0) before the handler runs so that
	 * syscalls are preemptible -- otherwise a syscall that calls
	 * kthread_yield (e.g. sys_yield) would context-switch the next
	 * thread in with PSTATE.I still set, leaving it unable to be
	 * preempted by the timer and starving other threads.
	 */
	if (ec == 0x15) {
		/* SPSR.M[3:0]: 0 = EL0t, 5 = EL1h.  Anything other than 5
		 * means the caller was at EL0 and the pointers it gave us
		 * are USER pointers that need bounds checking. */
		syscall_from_user = ((tf->spsr & 0xf) == 0) ? 1 : 0;
		__asm__ volatile ("msr daifclr, #2" ::: "memory");
		tf->x[0] = (uint64_t)syscall_dispatch(
				(long)tf->x[8],
				(long)tf->x[0], (long)tf->x[1], (long)tf->x[2],
				(long)tf->x[3], (long)tf->x[4], (long)tf->x[5]);
		syscall_from_user = 0;
		/* DEC/BSD reliable-signal delivery point: if a sys_kill
		 * landed while this syscall was running (or while the
		 * thread was blocked inside it), check_signals takes
		 * the default action -- currently always "exit" for
		 * fatal signals -- before we eret back to user. */
		check_signals();
		return;
	}

	kprintf("\n!! trap: %s  ec=0x%x (%s)  iss=0x%x\n",
		vec_name(vec_id), ec, ec_name(ec), iss);
	kprintf("   elr=0x%016lx  spsr=0x%08lx\n",
		(unsigned long)tf->elr, (unsigned long)tf->spsr);
	kprintf("   esr=0x%08lx  far=0x%016lx\n",
		(unsigned long)tf->esr, (unsigned long)tf->far);

	for (int i = 0; i < 30; i += 2) {
		kprintf("   x%-2d=0x%016lx  x%-2d=0x%016lx\n",
			i,     (unsigned long)tf->x[i],
			i + 1, (unsigned long)tf->x[i + 1]);
	}
	kprintf("   x30=0x%016lx  sp0=0x%016lx\n",
		(unsigned long)tf->x[30], (unsigned long)tf->sp_el0);

	/* Decode the faulting PC and walk the call stack via the saved
	 * (x29, x30) pair so the trace covers the faulting code, not
	 * the trap handler.  ksym_lookup turns each address into a
	 * function name + offset. */
	uint64_t off = 0;
	const char *name = ksym_lookup(tf->elr, &off);
	kprintf("   at %s+0x%lx\n",
		name ? name : "?", (unsigned long)off);
	kernel_backtrace_from(tf->x[29], tf->x[30]);

	/* A synchronous fault FROM EL0 is a user bug, not a kernel
	 * bug -- the right Unix response is to kill that thread with
	 * SIGSEGV and keep the kernel running.  vec_id 8 is "Lower
	 * EL using AArch64, synchronous" per the AArch64 vector
	 * table; the alternative VEC_SYNC_LO32 (AArch32) isn't
	 * reachable since we never enter EL0 in AArch32 mode. */
	if (vec_id == VEC_SYNC_LO64) {
		kprintf("   killing tid=%u with SIGSEGV\n",
			cur ? cur->tid : 0);
		if (cur) cur->sig_pending |= SIGBIT(SIGSEGV);
		sys_exit_impl();
		/* unreachable: sys_exit_impl never returns */
	}

	/* EL1 fault = real kernel bug.  Panic so we get the dump and
	 * stop before doing more damage. */
	kpanic("unhandled trap");
}

/*
 * arch/aarch64/thread.c -- AArch64 per-thread initial frame
 * =========================================================
 *
 * Same job as arch/arm/thread.c, different register file: builds
 * the synthetic frame (12 x 8-byte slots for callee-saved regs +
 * 2 x 8-byte slots for saved DAIF + alignment pad, 112 bytes
 * total) that arch/aarch64/switch.S's context_switch will pop
 * the first time the scheduler picks a freshly-created thread.
 * See switch.S for the layout diagram.
 */

#include <stdint.h>

#include "kappara/sched.h"

extern void thread_trampoline(void);

void *arch_thread_init_frame(void *stack_top, void (*fn)(void *), void *arg)
{
	uint64_t *sp = (uint64_t *)stack_top;
	sp -= 14;	/* 12 regs + DAIF + alignment pad = 112 bytes */
	sp[0]  = (uint64_t)(uintptr_t)fn;		/* x19 */
	sp[1]  = (uint64_t)(uintptr_t)arg;		/* x20 */
	sp[2]  = 0; sp[3]  = 0;				/* x21, x22 */
	sp[4]  = 0; sp[5]  = 0;				/* x23, x24 */
	sp[6]  = 0; sp[7]  = 0;				/* x25, x26 */
	sp[8]  = 0; sp[9]  = 0;				/* x27, x28 */
	sp[10] = 0;					/* x29 (fp) */
	sp[11] = (uint64_t)(uintptr_t)thread_trampoline;/* x30 (lr) */
	sp[12] = 0x80;	/* saved DAIF: bit 7 = PSTATE.I = IRQ masked.
			 *
			 * Required by phase-2 swtch: thread_trampoline
			 * calls sched_finish_switch() to release the
			 * cpu_thread_lock that swtch held across
			 * context_switch.  If a timer IRQ fired between
			 * trampoline entry and the unlock, the handler's
			 * own swtch invocation would deadlock on the
			 * still-held cpu_thread_lock.  The trampoline
			 * issues `daifclr, #2` only AFTER
			 * sched_finish_switch returns. */
	sp[13] = 0;	/* pad, keeps frame size 112 = 16-byte aligned */
	return sp;
}

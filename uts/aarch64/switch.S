/*
 * arch/aarch64/switch.S -- AArch64 kernel thread context switch
 * =============================================================
 *
 * What this file is
 * -----------------
 * The two assembly fragments the scheduler needs that you can't write
 * in C:
 *   context_switch    -- atomically save outgoing thread state and
 *                        restore incoming thread state
 *   thread_trampoline -- the bootstrap stub for a freshly-created
 *                        thread the first time the scheduler picks it
 *
 * The callee-saved subset trick
 * -----------------------------
 * AArch64's AAPCS (procedure call standard) says caller-saved
 * registers (x0..x18) and the temporaries are NOT preserved across
 * function calls.  Anyone who needs them already spilled them before
 * making a call.  That means a context switch only has to save the
 * callee-saved set:
 *
 *     x19, x20, x21, x22, x23, x24, x25, x26, x27, x28,
 *     x29 (frame pointer), x30 (link register == return address)
 *
 * Twelve 64-bit registers = 96 bytes, 16-byte aligned (AAPCS
 * requirement).  Saved as six stp pairs at the top of the outgoing
 * thread's kernel stack.  When we `mov sp, new_sp; ldp ...; ret`,
 * the `ret` jumps to whatever x30 was popped -- usually the inside of
 * the kthread_yield() call that the new thread last made (so it
 * returns from yield as if nothing happened), OR thread_trampoline
 * the very first time the thread runs.
 *
 *         outgoing stack (high to low)            initial stack of a
 *         after context_switch save:              freshly-created thread:
 *
 *         +----------------+ <-- old sp           +----------------+ <-- top of page
 *         |   x29, x30     |                      |   0,  trampoline|  x29=0, x30=tramp
 *         +----------------+                      +----------------+
 *         |   x27, x28     |                      |     0, 0       |
 *         +----------------+                      +----------------+
 *         |   x25, x26     |                      |     0, 0       |
 *         +----------------+                      +----------------+
 *         |   x23, x24     |                      |     0, 0       |
 *         +----------------+                      +----------------+
 *         |   x21, x22     |                      |     0, 0       |
 *         +----------------+                      +----------------+
 *         |   x19, x20     | <-- saved SP         |   fn,  arg     | <-- saved SP
 *         +----------------+                      +----------------+
 *           (lower stack...)                        (lower stack...)
 *
 * That second column is what kthread_create() lays down: arg goes
 * into x20 and fn into x19, with x30 pointing at thread_trampoline.
 *
 * The trampoline's job
 * --------------------
 * When the scheduler picks a new thread for the first time:
 *     ldp x19, x20, [sp, #0]       <- arg into x20, fn into x19
 *     ldp x21, x22, [sp, #16]      <- both zero
 *     ...
 *     ldp x29, x30, [sp, #80]      <- x29 = 0, x30 = thread_trampoline
 *     add sp, sp, #96
 *     ret                           <- jumps to thread_trampoline
 *
 * thread_trampoline then unmasks IRQs (a brand-new thread inherits
 * the masked-IRQ state of whatever context first scheduled it; if
 * that was an IRQ handler, DAIF.I=1 and we'd never preempt this
 * thread -- bug), does `blr x19` to call fn(arg), and if fn returns
 * tail-calls kthread_exit to clean up.
 *
 * void context_switch(void **save_sp, void *new_sp);
 *   x0 = address at which to save the outgoing SP
 *   x1 = SP of the thread we are switching to
 */

.section ".text"

/*
 * Per-thread DAIF is saved at [sp, #96] so each thread carries its
 * own IRQ-mask state across switches.  Without this, the CPU's DAIF
 * is just inherited from whoever ran last, which breaks the wake
 * path: a thread that slept with IRQs masked would otherwise
 * resume with whatever DAIF the waker happened to have, and a
 * thread that yielded from an IRQ handler (DAIF auto-masked on
 * entry) would propagate that masked state to every thread it
 * later switches to.
 */
.global context_switch
context_switch:
	sub	sp, sp, #112
	stp	x19, x20, [sp, #0]
	stp	x21, x22, [sp, #16]
	stp	x23, x24, [sp, #32]
	stp	x25, x26, [sp, #48]
	stp	x27, x28, [sp, #64]
	stp	x29, x30, [sp, #80]
	mrs	x2, daif
	str	x2, [sp, #96]

	mov	x2, sp
	str	x2, [x0]

	mov	sp, x1
	ldr	x2, [sp, #96]
	msr	daif, x2
	ldp	x19, x20, [sp, #0]
	ldp	x21, x22, [sp, #16]
	ldp	x23, x24, [sp, #32]
	ldp	x25, x26, [sp, #48]
	ldp	x27, x28, [sp, #64]
	ldp	x29, x30, [sp, #80]
	add	sp, sp, #112
	ret

/*
 * First-run entry for a thread created by kthread_create.  Initial
 * frame stores fn in x19 and arg in x20; arrives with DAIF.I set
 * (arch_thread_init_frame initial saved DAIF = 0x80, see thread.c).
 *
 *   1. bl sched_finish_switch
 *      Drains c->cpu_pending_requeue (push prev onto dispq with
 *      t_lockp transition) and releases cpu_thread_lock that was
 *      held across context_switch.  Must run with IRQs masked
 *      because we still hold cpu_thread_lock at entry; an IRQ
 *      that triggered swtch on this CPU would deadlock.
 *
 *   2. msr daifclr, #2
 *      Unmask IRQs.  Now preemptible.
 *
 *   3. mov x0, x20; blr x19
 *      Tail-call fn(arg).  If fn returns, fall through to
 *      kthread_exit.
 */
.global thread_trampoline
thread_trampoline:
	bl	sched_finish_switch
	msr	daifclr, #2
	mov	x0, x20
	blr	x19
	bl	kthread_exit
1:	wfe
	b	1b

/*
 * void aarch64_enter_userspace(uint64_t entry, uint64_t sp_el0, uint64_t arg);
 *   x0 = entry PC for EL0
 *   x1 = user-mode SP
 *   x2 = value to land in user x0 (function argument for `entry`)
 *
 * Sets SP_EL0 + ELR_EL1 + SPSR_EL1, copies arg into user x0, and
 * eret's into EL0t.  SPSR=0 means EL0t (M[3:0]=0), AArch64 (M[4]=0),
 * all DAIF=0 (IRQs unmasked from the moment the user runs).
 *
 * Does not return -- the eret leaves EL1 entirely.
 */
.global aarch64_enter_userspace
aarch64_enter_userspace:
	/*
	 * Mask IRQs across the ELR/SPSR write sequence.  The caller
	 * (a normal kthread) runs preemptible, so an IRQ taken between
	 * `msr elr_el1, x0` and the final `eret` would overwrite
	 * ELR_EL1 with the resume-PC inside this function; the IRQ's
	 * KERNEL_EXIT eret's back here fine, but our final eret then
	 * reads that stale ELR_EL1 and lands EL0 at a kernel address.
	 * Same shape as the trap_tail+0xc bug -- the bookkeeping cure
	 * is the same.  ERET pops SPSR.DAIF, so the user thread still
	 * starts with IRQs unmasked (xzr written above).
	 */
	msr	daifset, #2
	msr	sp_el0,   x1
	msr	elr_el1,  x0
	msr	spsr_el1, xzr
	mov	x0, x2		/* user x0 = arg                         */
	eret

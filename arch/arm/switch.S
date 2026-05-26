.syntax unified
.cpu cortex-a15
.arch armv7ve
.arm

.section .note.GNU-stack,"",%progbits

/*
 * arch/arm/switch.S -- ARMv7 kernel thread context switch
 * =======================================================
 *
 * ARMv7 cousin of arch/aarch64/switch.S.  Same idea: only the
 * AAPCS callee-saved set has to be preserved across a function
 * call, so context_switch only spills that set.
 *
 * ARMv7 AAPCS callee-saved registers:
 *
 *     r4, r5, r6, r7, r8, r9, r10, r11   -- 8 words
 *     sp (r13)                            -- handled implicitly
 *     lr (r14)                            -- return address
 *
 * Stack alignment
 * ---------------
 * AAPCS requires SP to be 8-byte aligned at public function call
 * boundaries.  Saving r4..r11 + lr = 9 words = 36 bytes is NOT
 * 8-aligned.  Pushing r12 (caller-saved, i.e. don't-care) as a
 * tenth word brings the frame to 40 bytes and keeps SP 8-aligned
 * without any extra adjustment trickery.
 *
 *     outgoing stack (high -> low) after the save:
 *
 *     +------------+ <-- old sp
 *     |   lr       |   +36
 *     +------------+
 *     |   r12      |   +32   (padding; restored garbage on resume)
 *     +------------+
 *     |   r11      |   +28
 *     |   r10      |   +24
 *     |   r9       |   +20
 *     |   r8       |   +16
 *     |   r7       |   +12
 *     |   r6       |   +8
 *     |   r5       |   +4
 *     |   r4       |   +0    <-- saved SP
 *     +------------+
 *
 * The "first time we run this thread" frame laid down by
 * arch_thread_init_frame() in arch/arm/thread.c looks the same,
 * with r4 = fn, r5 = arg, lr = thread_trampoline, everything else
 * zero.
 */

.section ".text"

.global context_switch
context_switch:
	push	{r4-r12, lr}
	str	sp, [r0]
	mov	sp, r1
	pop	{r4-r12, lr}
	bx	lr

/*
 * Bootstrap stub for a freshly-created thread.  On first entry r4
 * and r5 hold (fn, arg) -- the rest of the saved callee-saved set
 * came from the synthetic frame and is zero.  Unmask interrupts
 * (cpsie i clears CPSR.I) so this thread is preemptible, then
 * tail-call fn(arg).
 */
.global thread_trampoline
thread_trampoline:
	cpsie	i
	mov	r0, r5
	blx	r4
	bl	kthread_exit
1:	wfe
	b	1b

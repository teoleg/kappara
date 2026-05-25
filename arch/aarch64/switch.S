.section ".text"

/*
 * void context_switch(void **save_sp, void *new_sp);
 *   x0 = address at which to save the outgoing SP
 *   x1 = SP of the thread we are switching to
 *
 * Saves the AAPCS callee-saved set (x19..x29, lr) on the outgoing stack,
 * records the new SP, and pops the same set off the incoming stack.  The
 * `ret` at the bottom jumps to whatever LR was popped, so the first time
 * we switch into a freshly-created thread we land in thread_trampoline.
 */
.global context_switch
context_switch:
	sub	sp, sp, #96
	stp	x19, x20, [sp, #0]
	stp	x21, x22, [sp, #16]
	stp	x23, x24, [sp, #32]
	stp	x25, x26, [sp, #48]
	stp	x27, x28, [sp, #64]
	stp	x29, x30, [sp, #80]

	mov	x2, sp
	str	x2, [x0]

	mov	sp, x1
	ldp	x19, x20, [sp, #0]
	ldp	x21, x22, [sp, #16]
	ldp	x23, x24, [sp, #32]
	ldp	x25, x26, [sp, #48]
	ldp	x27, x28, [sp, #64]
	ldp	x29, x30, [sp, #80]
	add	sp, sp, #96
	ret

/*
 * First-run entry for a thread created by kthread_create.  Its prepared
 * initial frame stores fn in x19 and arg in x20; this trampoline unmasks
 * IRQs (so the new thread is preemptible) and tail-calls fn(arg).
 */
.global thread_trampoline
thread_trampoline:
	msr	daifclr, #2
	mov	x0, x20
	blr	x19
	bl	kthread_exit
1:	wfe
	b	1b

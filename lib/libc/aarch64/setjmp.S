/*
 * lib/libc/aarch64/setjmp.S -- AArch64 setjmp/longjmp
 *
 * The AAPCS64 callee-saved set is x19..x29 + LR (x30) + SP +
 * d8..d15.  Everything else is caller-saved.
 *
 * We deliberately do NOT save d8..d15 because kappara builds
 * userland with `-mgeneral-regs-only` and CPACR_EL1.FPEN is not
 * enabled for EL0 -- touching d-regs from a setjmp call would
 * trap (FP/SIMD access).  No compiled code in this libc uses
 * them, so omitting them is sound for the current ABI.
 *
 * Layout (matches setjmp.h's __jmp_buf):
 *   0..1   x19, x20
 *   2..3   x21, x22
 *   4..5   x23, x24
 *   6..7   x25, x26
 *   8..9   x27, x28
 *   10..11 x29 (fp), x30 (lr)
 *   12     sp
 *   13..21 reserved (would hold d8..d15 once FP is enabled)
 *
 * setjmp returns 0 on the first call.  longjmp(env, val) makes
 * the matching setjmp appear to return val (or 1 if val == 0,
 * matching ISO C semantics).
 */

	.text
	.global	setjmp
	.type	setjmp, %function
setjmp:
	stp	x19, x20, [x0, #0]
	stp	x21, x22, [x0, #16]
	stp	x23, x24, [x0, #32]
	stp	x25, x26, [x0, #48]
	stp	x27, x28, [x0, #64]
	stp	x29, x30, [x0, #80]
	mov	x1, sp
	str	x1,       [x0, #96]
	mov	x0, #0
	ret
	.size	setjmp, .-setjmp

	.global	longjmp
	.type	longjmp, %function
longjmp:
	ldp	x19, x20, [x0, #0]
	ldp	x21, x22, [x0, #16]
	ldp	x23, x24, [x0, #32]
	ldp	x25, x26, [x0, #48]
	ldp	x27, x28, [x0, #64]
	ldp	x29, x30, [x0, #80]
	ldr	x2,       [x0, #96]
	mov	sp, x2
	mov	x0, x1
	cmp	x0, #0
	b.ne	1f
	mov	x0, #1
1:
	ret
	.size	longjmp, .-longjmp

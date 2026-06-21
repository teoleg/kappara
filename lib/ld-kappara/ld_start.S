/*
 * lib/ld-kappara/ld_start.S -- entry stub for ld-kappara.so
 *
 * The kernel hands us control with the application's stack frame
 * already constructed: argc / argv / envp / auxv per SysV.  We walk
 * past argc + argv + envp to find the auxv pointer, hand it to
 * ld_main (C), which applies all relocations and returns the
 * application's entry point.  We then jump to it with the stack
 * unchanged so the application's crt0 reads argc/argv as usual.
 *
 * No callee-saved register dance is needed -- ld_main is leaf-ish
 * and we don't return.  We don't have a heap; ld_main's locals fit
 * in registers + the existing stack frame.
 */
	.section ".text._start","ax",%progbits
	.global _start
_start:
	/* Walk past argv to envp[0]. */
	ldr	x0, [sp]			/* argc                          */
	add	x1, sp, #16			/* &argv[0] (past argc + argv NUL) */
	add	x1, x1, x0, lsl #3		/* &envp[0]                       */

	/* Walk envp to NUL terminator. */
1:	ldr	x2, [x1], #8
	cbnz	x2, 1b

	/* x1 now points at auxv[0].  Call ld_main(auxv). */
	mov	x0, x1
	bl	ld_main

	/* ld_main returns app entry in x0.  Jump to it without
	 * touching the stack -- the app's crt0 reads argc / argv
	 * from [sp] / [sp+8]. */
	br	x0

/*
 * lib/ld-kappara/ld_start.S -- minimal dynamic linker (stage 5)
 *
 * The kernel loads us at LD_VA (0x30000000) and points the trap
 * frame's ELR/SP at our _start with the application's stack frame
 * already constructed:
 *
 *   sp + 0           argc
 *   sp + 8           argv[0]
 *   ...              argv[argc-1]
 *   sp + 8*(argc+1)  NULL (argv terminator)
 *   sp + ...         envp[0] (none today)
 *   sp + ...         NULL (envp terminator)
 *                    auxv[0].a_type
 *                    auxv[0].a_un.a_val
 *   ...              AT_NULL (a_type == 0)
 *
 * Stage 5 deliverable: walk the auxv for AT_ENTRY (a_type == 9)
 * and branch to it.  The app's crt0 reads argc/argv from [sp] /
 * [sp+8] -- we leave the stack untouched.
 *
 * Stage 6 will grow this to walk the app's PT_DYNAMIC (located
 * via AT_PHDR / AT_PHNUM), process DT_NEEDED entries, mmap each
 * shared object, and apply cross-DSO relocations.
 *
 * No registers other than x0..x5 (scratch) are clobbered; the
 * AArch64 ABI says x19..x30 are caller-saved at entry, so the
 * app sees a clean register file.
 */
	.section ".text._start","ax",%progbits
	.global _start
_start:
	/* Walk past argv to envp[0]. */
	ldr	x0, [sp]		/* argc */
	add	x1, sp, #16		/* &argv[0] : skip argc + argv[argc]=NULL slot */
	add	x1, x1, x0, lsl #3	/* sp + 8 + 8*argc + 8 = past argv NULL */

	/* Walk past envp to auxv[0]. */
1:	ldr	x2, [x1], #8
	cbnz	x2, 1b

	/* Walk auxv looking for AT_ENTRY (a_type == 9).  Always read
	 * both type+value per iteration (keeps x1 incrementing in pairs);
	 * cbnz on type catches the AT_NULL terminator. */
	mov	x3, #0			/* x3 = app entry accumulator */
2:	ldr	x4, [x1], #8		/* a_type */
	ldr	x5, [x1], #8		/* a_un.a_val */
	cmp	x4, #9			/* AT_ENTRY */
	csel	x3, x5, x3, eq
	cbnz	x4, 2b

	/* Hand control to the application's entry point.  Stack is
	 * unchanged; argc / argv / envp / auxv all visible to the
	 * app's crt0. */
	br	x3

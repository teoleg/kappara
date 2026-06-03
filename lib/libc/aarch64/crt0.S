/*
 * crt0.S -- C runtime entry for kappara EL0 programs
 * _start is called by the kernel at EL0 with SP pointing at the
 * exec stack frame laid out by sys_execve_impl:
 *
 *   [SP+0]    argc   (uint64_t)
 *   [SP+8]    argv[0] pointer
 *   [SP+16]   argv[1] pointer
 *   ...
 *   [SP+8*(argc+1)]  NULL terminator
 *
 * We load argc from [SP] and set argv = SP+8, then call main().
 */
	.section ".text._start","ax",%progbits
	.global _start
_start:
	ldr	x0, [sp]	/* argc                    */
	add	x1, sp, #8	/* argv = &stack_word[1]   */
	bl	main
	/* exit(return_value) -- x0 already has main's retval */
	mov	x8, #16		/* SYS_exit                */
	svc	#0
	b	.		/* unreachable             */

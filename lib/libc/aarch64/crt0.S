/*
 * crt0.S -- C runtime entry for kappara EL0 programs
 * _start is called by the kernel at EL0 with a clean register state.
 * We set up argc=0/argv=NULL, call main(), then exit.
 */
	.section ".text._start","ax",%progbits
	.global _start
_start:
	mov	x0, #0		/* argc = 0          */
	mov	x1, xzr	/* argv = NULL        */
	bl	main
	/* x0 = main's return value; pass to exit (status ignored for now) */
	mov	x8, #16		/* SYS_exit           */
	svc	#0
	b	.		/* unreachable        */

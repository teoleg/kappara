.syntax unified
.cpu cortex-a15
.arch armv7ve
.arm

.section .note.GNU-stack,"",%progbits

.section ".text.boot"
.global _start

_start:
	@ Only core 0 boots; everyone else parks on WFE.
	mrc	p15, 0, r0, c0, c0, 5		@ MPIDR
	and	r0, r0, #0xff
	cmp	r0, #0
	bne	.Lpark

	@ Dispatch on current PE mode.
	mrs	r0, cpsr
	and	r1, r0, #0x1f
	cmp	r1, #0x1a			@ HYP mode
	bne	.Lin_svc

	@ We're in HYP.  Build a return frame to SVC mode, IRQ+FIQ masked.
	mov	r0, #0x13			@ SVC mode
	orr	r0, r0, #(1 << 6) | (1 << 7)	@ I=1, F=1
	msr	spsr_hyp, r0
	ldr	r0, =.Lin_svc
	msr	elr_hyp, r0
	eret

.Lpark:
	wfe
	b	.Lpark

.Lin_svc:
	@ Stack just below the load address.
	ldr	sp, =_start

	@ Zero BSS.
	ldr	r0, =__bss_start
	ldr	r1, =__bss_end
	mov	r2, #0
.Lbss:
	cmp	r0, r1
	bge	.Lcall
	str	r2, [r0], #4
	b	.Lbss

.Lcall:
	bl	kmain
.Lhang:
	wfe
	b	.Lhang

/*
 * arch/arm/boot.S -- ARMv7-A kernel entry point
 * =============================================
 *
 * What this file is
 * -----------------
 * The ARMv7 cousin of arch/aarch64/boot.S.  Same job: leave the CPU
 * in a clean state -- single core, supervisor mode (SVC), stack
 * pointer set, BSS zeroed -- and call kmain.
 *
 * ARMv7 processor modes (CPSR.M[4:0])
 * -----------------------------------
 *
 *     USR  0x10  user                  (eventual EL0-equivalent)
 *     FIQ  0x11  fast interrupt
 *     IRQ  0x12  ordinary interrupt
 *     SVC  0x13  supervisor            <-- kernel lives here
 *     MON  0x16  secure monitor        (TrustZone)
 *     ABT  0x17  data/prefetch abort
 *     HYP  0x1A  hypervisor            <-- QEMU enters here on a15/a7
 *     UND  0x1B  undefined instruction
 *     SYS  0x1F  system (privileged, shares USR registers)
 *
 * Boot dispatch
 * -------------
 *
 *      entry
 *       |
 *       v
 *      MPIDR.aff0 != 0 ?  --yes--> spin on wfe (.Lpark)
 *       |
 *      CPSR.M == HYP ?    --yes--> .Lfrom_hyp:
 *       |                            SPSR_hyp <- SVC mode + I/F masked
 *       |                            ELR_hyp  <- .Lin_svc
 *       |                            eret
 *       v
 *      .Lin_svc:
 *        sp = _start
 *        zero BSS
 *        bl kmain
 *
 * The HYP -> SVC drop
 * -------------------
 * Cortex-A15 / Cortex-A7 with the Virt extensions boot in HYP on QEMU
 * `-M virt`.  HYP is a separate privilege level; from there an `eret`
 * returns to whichever mode SPSR_hyp says, with PC = ELR_hyp.  By
 * loading SVC + masked-IRQ into SPSR_hyp and the address of .Lin_svc
 * into ELR_hyp, the eret cleanly drops us into SVC mode running the
 * kernel.  On boards that boot us straight in SVC (no Virt) the
 * dispatch falls through to .Lin_svc directly.
 */

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

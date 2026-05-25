.syntax unified
.cpu cortex-a15
.arch armv7ve
.arm

.section .note.GNU-stack,"",%progbits

/*
 * arch/arm/vectors.S -- ARMv7-A exception vector table and trap entry
 * =====================================================================
 *
 * What this file is
 * -----------------
 * The 8-entry exception vector table the CPU jumps to whenever
 * something interesting happens (an undefined instruction, a data
 * abort, a hardware IRQ, a software syscall, ...) plus the assembly
 * trampolines that turn that hardware-defined "you're now in mode X
 * with a tiny banked register set" entry into a structured C call
 * with a saved trap frame on the SVC stack.
 *
 * Why it exists
 * -------------
 * Unlike AArch64 (which has a fixed 16-slot, 0x80-byte-per-entry vector
 * table tied to one exception level), ARMv7 has eight 4-byte vector
 * entries and switches into one of eight CPU "modes" depending on the
 * cause.  Each mode has its own banked SP/LR (and FIQ also banks
 * r8..r12) so the hardware always has somewhere to scribble even when
 * the previous mode's stack is unknown.  We use that just long enough
 * to atomically migrate to SVC mode and build a uniform frame there.
 *
 * Vector table layout (programmed into VBAR by trap_init)
 * -------------------------------------------------------
 *
 *     offset  cause                target mode  LR_<m> = ...
 *     ------  -------------------  -----------  --------------------
 *     0x00    Reset                SVC          PC = 0
 *     0x04    Undefined instr      UND          next instr + 4
 *     0x08    SVC (svc #imm)       SVC          next instr        (= return)
 *     0x0C    Prefetch abort       ABT          aborted instr + 4
 *     0x10    Data abort           ABT          aborted instr + 8
 *     0x14    Reserved / HYP trap  -            -
 *     0x18    IRQ                  IRQ          next instr + 4
 *     0x1C    FIQ                  FIQ          next instr + 4
 *
 * Each entry is exactly one branch instruction (4 bytes); the real
 * work happens in the VEC_HANDLER below.  The whole table is 32 bytes
 * and must start on at least a 32-byte boundary (VBAR's low bits read
 * as zero).
 *
 * Why the "lr_adj" per vector
 * ---------------------------
 * Different exceptions stash subtly different return addresses in
 * LR_<mode>.  For IRQ/FIQ/Undef we want LR-4 to re-execute the
 * instruction after the trapping one; for DAbt we want LR-8 to retry
 * the load/store; for SVC we want LR unchanged (it already points to
 * the next instruction).  The hardware does NOT normalise these, so
 * the handler does it once, up front, before saving the frame.
 *
 * The save sequence
 * -----------------
 * Each VEC_HANDLER does, in order:
 *
 *   sub  lr, lr, #lr_adj         ; normalise LR to "the address we
 *                                   want to return to"
 *   srsdb sp!, #0x13             ; push LR + SPSR onto SP_svc
 *                                   (predecrement, write-back)
 *   cps  #0x13                   ; switch to SVC mode -- now SP=SP_svc
 *   push {r0-r12, lr}            ; save the GP regs + LR_svc
 *
 * After this, SP_svc points at a uniform trap frame regardless of
 * which mode we entered from:
 *
 *     SVC stack (grows down):
 *
 *     +---------------------+ <-- old SP_svc
 *     |       SPSR_<m>      |   +60     CPSR at time of trap
 *     +---------------------+
 *     |       LR_<m> (adj)  |   +56     return PC
 *     +---------------------+
 *     |       lr_svc        |   +52     LR_svc at time of save
 *     +---------------------+
 *     |       r12           |   +48
 *     |       r11           |   +44
 *     |       ...           |
 *     |       r0            |   +0      <-- new SP_svc, == frame ptr
 *     +---------------------+
 *
 * mov r0, sp gives the trap_dump() C function the address of r0 inside
 * the frame (matching struct arm_trap_frame in arch/arm/trap.c), and
 * r1 carries the vec_id (0..7) so we know which slot fired.
 *
 * Return from exception
 * ---------------------
 * After bl trap_dispatch the handler tail unwinds:
 *
 *     pop {r0-r12, lr}    ; restore GPRs, sp now at saved (LR, SPSR)
 *     rfeia sp!           ; PC <- [sp], CPSR <- [sp+4], sp += 8
 *
 * rfeia is the symmetric counterpart of srsdb: it pops PC and CPSR
 * from the stack and jumps, simultaneously restoring the previous
 * privilege/interrupt-mask state.  trap_dispatch is responsible
 * for choosing to return (IRQ path) or to never return (kpanic on
 * unhandled synchronous faults), so this tail is only reached
 * after a successful IRQ.
 */

.section ".text"

.macro VEC_HANDLER name, lr_adj, vec_id
\name:
	.if \lr_adj
	sub	lr, lr, #\lr_adj
	.endif
	srsdb	sp!, #0x13		@ push LR + SPSR onto SP_svc
	cps	#0x13			@ now running in SVC mode
	push	{r0-r12, lr}
	mov	r0, sp			@ trap frame pointer
	mov	r1, #\vec_id
	bl	trap_dispatch
	pop	{r0-r12, lr}
	rfeia	sp!			@ pop saved PC + CPSR, eret
.endm

.balign 32
.global arm_vectors
arm_vectors:
	b	_vec_reset
	b	_vec_undef
	b	_vec_svc
	b	_vec_pabt
	b	_vec_dabt
	b	_vec_reserved
	b	_vec_irq
	b	_vec_fiq

VEC_HANDLER _vec_reset,    0, 0
VEC_HANDLER _vec_undef,    4, 1
VEC_HANDLER _vec_svc,      0, 2
VEC_HANDLER _vec_pabt,     4, 3
VEC_HANDLER _vec_dabt,     8, 4
VEC_HANDLER _vec_reserved, 0, 5
VEC_HANDLER _vec_irq,      4, 6
VEC_HANDLER _vec_fiq,      4, 7

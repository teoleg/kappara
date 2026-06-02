/*
 * arch/aarch64/vectors.S -- AArch64 EL1 exception vector table
 * ============================================================
 *
 * What this file is
 * -----------------
 * The 16-slot AArch64 vector table programmed into VBAR_EL1 by
 * trap_init(), plus the per-slot trampolines that save a uniform
 * 288-byte trap frame on the kernel stack and dispatch to the C
 * trap_dispatch() function.
 *
 * AArch64 vector table shape
 * --------------------------
 * Unlike ARMv7's eight 4-byte entries, AArch64 gives each vector a
 * full 0x80 (128 bytes / 32 instructions).  That's enough room for
 * the GP-register save sequence to be inlined per-slot, so each
 * exception lands directly in a self-contained handler without an
 * extra indirection.  The table is 2 KB total and must be 2 KB
 * aligned.
 *
 * Sixteen slots arranged as 4 categories x 4 causes:
 *
 *     offset  category               cause
 *     ------  ---------------------  -----------
 *     0x000   Current EL, SP_EL0     synchronous
 *     0x080                          IRQ
 *     0x100                          FIQ
 *     0x180                          SError
 *     0x200   Current EL, SP_ELx     synchronous   <- we mostly hit this
 *     0x280                          IRQ           <- timer fires here
 *     0x300                          FIQ
 *     0x380                          SError
 *     0x400   Lower EL, AArch64      synchronous   <- syscalls from EL0
 *     0x480                          IRQ
 *     0x500                          FIQ
 *     0x580                          SError
 *     0x600   Lower EL, AArch32      synchronous   <- we don't use these
 *     0x680                          IRQ
 *     0x700                          FIQ
 *     0x780                          SError
 *
 * Trap frame layout (288 bytes, 16-byte aligned)
 * ----------------------------------------------
 *   [0x000..0x0F0)  x0..x29  as 15 stp pairs   (general purpose regs)
 *   [0x0F0..0x100)  x30, sp_el0                (link reg, user SP)
 *   [0x100..0x110)  elr_el1, spsr_el1          (return PC + saved PSTATE)
 *   [0x110..0x120)  esr_el1, far_el1           (syndrome + fault address)
 *
 * Matches `struct trap_frame` in include/kappara/trap.h.
 *
 * Macros
 * ------
 *   KERNEL_ENTRY -- sub sp, push every reg via stp pairs, snapshot the
 *                   system regs (sp_el0, elr_el1, spsr_el1, esr_el1,
 *                   far_el1) into the frame.
 *
 *   KERNEL_EXIT  -- the mirror image: restore system regs (just elr
 *                   and spsr; esr/far are read-only-ish), pop the GPRs,
 *                   `add sp, sp, #288`, `eret`.  Restoring SPSR_EL1
 *                   and ELR_EL1 before eret is how the CPU returns to
 *                   the interrupted code with the same PSTATE.
 *
 *   VECTOR id    -- one full vector slot: .balign 128, KERNEL_ENTRY,
 *                   mov r0=sp (frame), r1=id (which slot), branch to
 *                   the shared trap_tail (bl trap_dispatch; KERNEL_EXIT).
 *                   Each slot is ~108 bytes of code padded to 128 by
 *                   .balign before the next.
 */

.section ".text"

.macro KERNEL_ENTRY
	sub	sp, sp, #288
	stp	x0,  x1,  [sp, #16*0]
	stp	x2,  x3,  [sp, #16*1]
	stp	x4,  x5,  [sp, #16*2]
	stp	x6,  x7,  [sp, #16*3]
	stp	x8,  x9,  [sp, #16*4]
	stp	x10, x11, [sp, #16*5]
	stp	x12, x13, [sp, #16*6]
	stp	x14, x15, [sp, #16*7]
	stp	x16, x17, [sp, #16*8]
	stp	x18, x19, [sp, #16*9]
	stp	x20, x21, [sp, #16*10]
	stp	x22, x23, [sp, #16*11]
	stp	x24, x25, [sp, #16*12]
	stp	x26, x27, [sp, #16*13]
	stp	x28, x29, [sp, #16*14]
	mrs	x10, sp_el0
	stp	x30, x10, [sp, #16*15]
	mrs	x10, elr_el1
	mrs	x11, spsr_el1
	stp	x10, x11, [sp, #16*16]
	mrs	x10, esr_el1
	mrs	x11, far_el1
	stp	x10, x11, [sp, #16*17]
.endm

.macro KERNEL_EXIT
	/*
	 * Mask IRQs across the epilogue.  The SVC handler in
	 * trap_dispatch unmasks IRQs while running the syscall body
	 * (so syscalls are preemptible), and returns here with IRQs
	 * still unmasked.  If we don't re-mask, an IRQ taken between
	 *     msr elr_el1, x10        // <-- writes user PC
	 *     msr spsr_el1, x11       // <-- writes user PSTATE
	 *     ...
	 *     eret
	 * lets the hardware OVERWRITE ELR_EL1 with the resume-here
	 * kernel PC.  The IRQ's own KERNEL_EXIT eret's back into the
	 * middle of OUR epilogue at that resume-here address -- which
	 * is fine for resuming kernel execution, but the eret at the
	 * end of our epilogue then reads the still-stale ELR_EL1 (the
	 * outer epilogue only writes ELR_EL1 once, before the IRQ
	 * window) and jumps EL0 to a kernel address.  Instruction
	 * abort lower EL at trap_tail+0xc -- the canonical symptom of
	 * this race.  Masking here makes the whole epilogue atomic
	 * with respect to async exceptions.
	 */
	msr	daifset, #2
	ldp	x10, x11, [sp, #16*16]
	msr	elr_el1,  x10
	msr	spsr_el1, x11
	ldp	x30, x10, [sp, #16*15]
	msr	sp_el0,   x10
	ldp	x0,  x1,  [sp, #16*0]
	ldp	x2,  x3,  [sp, #16*1]
	ldp	x4,  x5,  [sp, #16*2]
	ldp	x6,  x7,  [sp, #16*3]
	ldp	x8,  x9,  [sp, #16*4]
	ldp	x10, x11, [sp, #16*5]
	ldp	x12, x13, [sp, #16*6]
	ldp	x14, x15, [sp, #16*7]
	ldp	x16, x17, [sp, #16*8]
	ldp	x18, x19, [sp, #16*9]
	ldp	x20, x21, [sp, #16*10]
	ldp	x22, x23, [sp, #16*11]
	ldp	x24, x25, [sp, #16*12]
	ldp	x26, x27, [sp, #16*13]
	ldp	x28, x29, [sp, #16*14]
	add	sp, sp, #288
	eret
.endm

.macro VECTOR id
	.balign 128
	KERNEL_ENTRY
	mov	x0, sp
	mov	x1, #\id
	b	trap_tail
.endm

	.balign 2048
	.global vectors
vectors:
	VECTOR 0	// Current EL, SP_EL0, synchronous
	VECTOR 1	// Current EL, SP_EL0, IRQ
	VECTOR 2	// Current EL, SP_EL0, FIQ
	VECTOR 3	// Current EL, SP_EL0, SError
	VECTOR 4	// Current EL, SP_ELx, synchronous
	VECTOR 5	// Current EL, SP_ELx, IRQ
	VECTOR 6	// Current EL, SP_ELx, FIQ
	VECTOR 7	// Current EL, SP_ELx, SError
	VECTOR 8	// Lower EL, AArch64, synchronous
	VECTOR 9	// Lower EL, AArch64, IRQ
	VECTOR 10	// Lower EL, AArch64, FIQ
	VECTOR 11	// Lower EL, AArch64, SError
	VECTOR 12	// Lower EL, AArch32, synchronous
	VECTOR 13	// Lower EL, AArch32, IRQ
	VECTOR 14	// Lower EL, AArch32, FIQ
	VECTOR 15	// Lower EL, AArch32, SError

trap_tail:
	bl	trap_dispatch
	KERNEL_EXIT

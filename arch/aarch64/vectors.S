.section ".text"

// Trap frame layout (288 bytes, 16-byte aligned):
//   [0x000..0x0F0)  x0..x29  as 15 stp pairs
//   [0x0F0..0x100)  x30, sp_el0
//   [0x100..0x110)  elr_el1, spsr_el1
//   [0x110..0x120)  esr_el1, far_el1

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

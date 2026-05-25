.section ".text.boot"
.global _start

_start:
	mrs	x0, mpidr_el1
	and	x0, x0, #0xff
	cbnz	x0, .Lpark

	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	cmp	x0, #3
	b.eq	.Lfrom_el3
	cmp	x0, #2
	b.eq	.Lfrom_el2
	b	.Lin_el1

.Lpark:
	wfe
	b	.Lpark

.Lfrom_el3:
	mov	x0, #0x05B1
	msr	scr_el3, x0
	mov	x0, #0x03C9
	msr	spsr_el3, x0
	adr	x0, .Lfrom_el2
	msr	elr_el3, x0
	eret

.Lfrom_el2:
	mrs	x0, cnthctl_el2
	orr	x0, x0, #(1 << 0)
	orr	x0, x0, #(1 << 1)
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr

	mov	x0, #(1 << 31)
	msr	hcr_el2, x0

	mov	x0, #0x0800
	movk	x0, #0x30d0, lsl #16
	msr	sctlr_el1, x0

	mov	x0, #0x03C5
	msr	spsr_el2, x0
	adr	x0, .Lin_el1
	msr	elr_el2, x0
	eret

.Lin_el1:
	ldr	x0, =_start
	mov	sp, x0

	ldr	x0, =__bss_start
	ldr	x1, =__bss_end
.Lbss:
	cmp	x0, x1
	b.ge	.Lcall
	str	xzr, [x0], #8
	b	.Lbss

.Lcall:
	bl	kmain
.Lhang:
	wfe
	b	.Lhang

.section ".text.boot"
.global _start

_start:
	mrs	x0, mpidr_el1
	and	x0, x0, #0xff
	cbz	x0, .Lprimary
.Lpark:
	wfe
	b	.Lpark

.Lprimary:
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

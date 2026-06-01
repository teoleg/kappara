/*
 * arch/aarch64/boot.S -- AArch64 kernel entry point
 * =================================================
 *
 * What this file is
 * -----------------
 * The very first code that runs after QEMU's loader (or the firmware
 * on a real Pi 3) jumps into our kernel image.  Its job is to leave
 * the CPU in a clean, well-known state that the C kmain() can
 * actually run on: one core, a known exception level (EL1), a stack
 * pointer, and a zeroed BSS.  Nothing else.
 *
 * Why we boot on an exception level at all
 * ----------------------------------------
 * AArch64 splits privilege into four levels:
 *
 *     EL3  Secure Monitor (TrustZone, firmware)
 *     EL2  Hypervisor
 *     EL1  Kernel  <-- where we want to run
 *     EL0  Userspace
 *
 * QEMU's raspi3b enters our kernel at EL2.  Some boards / configs
 * enter at EL3.  We handle both and end up at EL1 either way:
 *
 *     entry
 *      |
 *      v
 *     EL3? --yes--> .Lfrom_el3:  set SCR_EL3, SPSR_EL3 -> EL2h, ERET
 *      |                                |
 *      |                                v
 *     EL2? --yes--> .Lfrom_el2:  set CNTHCTL_EL2, HCR_EL2.RW=1,
 *      |                          SCTLR_EL1 (RES1 bits, MMU off),
 *      |                          SPSR_EL2 -> EL1h, ERET
 *      |                                |
 *      v                                v
 *     EL1: .Lin_el1:  set SP, zero BSS, bl kmain
 *
 * Secondary cores
 * ---------------
 * The Pi 3 has four Cortex-A53 cores; QEMU starts all of them at
 * _start.  Only core 0 (MPIDR_EL1.Aff0 == 0) does anything useful;
 * the rest spin on `wfe` so they don't trample on our single-core
 * state.  We'll come back for them once we have SMP plumbing.
 *
 * Magic constants
 * ---------------
 *   SCR_EL3   = 0x05B1
 *               NS=1, IRQ=0, FIQ=0, EA=0, [5:4]=RES1, SMD=1, HCE=1, RW=1
 *               (Non-secure EL2 in AArch64, HVC enabled, SMC disabled.)
 *   SPSR_EL3  = 0x03C9 = EL2h + D=I=F=A=1   (mask all, return to EL2h)
 *   SPSR_EL2  = 0x03C5 = EL1h + D=I=F=A=1   (mask all, return to EL1h)
 *   HCR_EL2.RW (bit 31) = 1: EL1 runs as AArch64.
 *   SCTLR_EL1 = 0x30d00800: just the architecture-mandated RES1 bits,
 *               with MMU (M), data cache (C), and instruction cache
 *               (I) all OFF.  mmu_init() flips those later.
 */

.section ".text.boot"
.global _start

_start:
	/* Only core 0 of cluster 0 proceeds; the others park. */
	mrs	x0, mpidr_el1
	and	x0, x0, #0xff
	cbnz	x0, .Lpark

	/* Branch on the EL we entered at: 3 -> 2 -> 1, or 2 -> 1, or stay. */
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	cmp	x0, #3
	b.eq	.Lfrom_el3
	cmp	x0, #2
	b.eq	.Lfrom_el2
	b	.Lin_el1

.Lpark:
	/* Park-with-spin-table.  Each secondary core (1..3) checks its
	 * own slot in smp_release[] (defined in C).  If it's still
	 * zero, WFE until something happens; if it's non-zero, branch
	 * to that address (which sets up the per-core MMU / stack and
	 * calls into secondary_main).
	 *
	 * WFE here (not WFI) so that an SEV from core 0 -- issued by
	 * the wake function -- actually wakes us; WFI would also work
	 * but requires routing an IRQ.  The spin-then-WFE shape is
	 * the same boot protocol real Pi 3 firmware uses for its own
	 * spin-table at PA 0xD8, so when we eventually run on metal we
	 * just need to hook into that. */
	mrs	x0, mpidr_el1
	and	x0, x0, #0xff	/* cpu id 1..3 */
	adr	x1, smp_release
	ldr	x2, [x1, x0, lsl #3]
	cbz	x2, 1f
	br	x2		/* jump to per-core entry */
1:	wfe
	b	.Lpark

/*
 * Spin-table release addresses, one slot per core.  Slot 0 is
 * unused (core 0 doesn't park).  Slots 1..3 are written by
 * smp_wake_secondary() from C; the secondary core polling .Lpark
 * sees the new value, jumps to it, and never returns here.
 *
 * Placed in .data so it's BSS-zero-initialised at boot.  The label
 * is global so the C code can address it.
 */
	.section ".data"
	.balign 8
	.global	smp_release
smp_release:
	.quad	0
	.quad	0
	.quad	0
	.quad	0
	.global smp_stacks
smp_stacks:
	.quad	0	/* slot 0 unused (core 0 has main stack) */
	.quad	0
	.quad	0
	.quad	0
	.section ".text"

/*
 * Entry point branched to from the spin-table by a freshly-woken
 * secondary core.  Sets a per-core SP from smp_stacks[], then drops
 * into secondary_main (C).  Stays at EL2 with MMU off for now -- the
 * MMU + EL1 transition for secondary cores is a follow-up.  All we
 * want here is "the core is alive and executing C code."
 *
 * The C function never returns; if it ever does, fall back to wfi.
 */
.global secondary_start
secondary_start:
	mrs	x0, mpidr_el1
	and	x0, x0, #0xff
	adr	x1, smp_stacks
	ldr	x2, [x1, x0, lsl #3]
	mov	sp, x2
	bl	secondary_main
1:	wfi
	b	1b

	/* EL3 -> EL2 transition (only used if firmware drops us in EL3). */
.Lfrom_el3:
	mov	x0, #0x05B1
	msr	scr_el3, x0
	mov	x0, #0x03C9
	msr	spsr_el3, x0
	adr	x0, .Lfrom_el2
	msr	elr_el3, x0
	eret

	/* EL2 -> EL1 transition (raspi3b default entry). */
.Lfrom_el2:
	/* Let EL1/EL0 access the physical and virtual counters. */
	mrs	x0, cnthctl_el2
	orr	x0, x0, #(1 << 0)		/* EL1PCTEN */
	orr	x0, x0, #(1 << 1)		/* EL1PCEN  */
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr

	/* EL1 runs as AArch64 (HCR_EL2.RW = 1). */
	mov	x0, #(1 << 31)
	msr	hcr_el2, x0

	/* SCTLR_EL1 = res-1 bits only, MMU/caches off; mmu_init enables them. */
	mov	x0, #0x0800
	movk	x0, #0x30d0, lsl #16
	msr	sctlr_el1, x0

	mov	x0, #0x03C5			/* EL1h + DAIF masked */
	msr	spsr_el2, x0
	adr	x0, .Lin_el1
	msr	elr_el2, x0
	eret

.Lin_el1:
	/*
	 * Stack just below the load address.  We have 0x80000 bytes
	 * (512 KB) of address space from 0x0 to 0x80000 to use, which
	 * is plenty for the kernel's bring-up stack.
	 */
	ldr	x0, =_start
	mov	sp, x0

	/*
	 * Zero BSS in 8-byte strides.  __bss_start/__bss_end come from
	 * the linker script and are 8-byte aligned.
	 */
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

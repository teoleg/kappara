/*
 * uts/virt/boot.S -- QEMU `virt` machine entry point
 *
 * The virt machine is much simpler than raspi3b at boot time:
 *
 *   - QEMU loads the kernel at the start of RAM (PLAT_RAM_BASE
 *     = 0x40000000); -kernel does an in-place load and jumps to it.
 *   - We enter at EL2 (default for `-M virt -cpu cortex-a72`); EL3
 *     possible with -machine virtualization=on.  We drop to EL1
 *     either way.
 *   - Secondaries are NOT started by the firmware -- they sit in PSCI
 *     WFI until the kernel makes the CPU_ON HVC call.  No spin-table.
 *     SMP wiring is a separate later step; for now CPU 0 runs alone.
 *
 * Same shape as uts/aarch64/boot.S core-0 path otherwise: branch on
 * CurrentEL, set up SCTLR_EL1's RES1 bits with MMU/caches off, point
 * sp_el1 at our BSS-allocated kernel stack, eret to .Lin_el1, zero
 * the BSS, call kmain().
 */

.section ".text.boot"
.global _start

/* Linux ARM64 boot protocol header (Documentation/arm64/booting.rst).
 * QEMU's -kernel loader on aarch64 looks at offset 0x38 for the
 * "ARM\x64" magic to identify the image; without the header it can't
 * tell where to place us, so we copy the header layout Linux's
 * arch/arm64/kernel/head.S uses.  The first 4 bytes "MZ" are the
 * MS-DOS magic so EFI can also boot us as a PE/COFF binary (we
 * skip the PE COFF body since QEMU doesn't need it). */
_start:
	add	x13, x18, #0x16		/* encodes as "MZ" little-endian: PE magic */
	b	.Lreal_start
	.quad	0			/* image_load_offset_le */
	.quad	0			/* image_size_le (0 = inherit from -kernel) */
	.quad	0			/* image_flags_le */
	.quad	0
	.quad	0
	.quad	0
	.ascii	"ARM\x64"		/* magic at offset 0x38 */
	.long	0			/* PE COFF offset (0 = none) */

.Lreal_start:
	/* Only core 0 of cluster 0 proceeds.  Should be the only one
	 * here on virt since secondaries are PSCI-parked, but defend
	 * anyway -- a future -bios or fw_cfg boot might start more. */
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
	wfi
	b	.Lpark

.Lfrom_el3:
	/* SCR_EL3 = NS=1, [5:4]=RES1, SMD=1, HCE=1, RW=1.
	 * SPSR_EL3 -> EL2h, all DAIF masked.  ERET into .Lfrom_el2. */
	mov	x0, #0x05B1
	msr	scr_el3, x0
	mov	x0, #0x03C9
	msr	spsr_el3, x0
	adr	x0, .Lfrom_el2
	msr	elr_el3, x0
	eret

.Lfrom_el2:
	/* Stack pointer for EL1.  Use sp_el1 so the eret lands with
	 * the stack already valid -- no extra mov in the EL1 epilogue.
	 * adrp + add (not plain adr): the kernel + BSS can span >1 MB
	 * now, putting stack_top out of adr's +/-1 MB reach. */
	adrp	x0, stack_top
	add	x0, x0, :lo12:stack_top
	msr	sp_el1, x0

	/* EL1/EL0 can read the physical and virtual counters. */
	mrs	x0, cnthctl_el2
	orr	x0, x0, #(1 << 0)	/* EL1PCTEN */
	orr	x0, x0, #(1 << 1)	/* EL1PCEN  */
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr

	/* HCR_EL2.RW = 1: EL1 runs AArch64. */
	mov	x0, #(1 << 31)
	msr	hcr_el2, x0

	/* SCTLR_EL1: just the RES1 bits, MMU/caches OFF.  mmu_init() in
	 * C turns them on. */
	mov	x0, #0x0800
	movk	x0, #0x30d0, lsl #16
	msr	sctlr_el1, x0

	/* SPSR_EL2 = EL1h, all DAIF masked.  ERET to .Lin_el1. */
	mov	x0, #0x03C5
	msr	spsr_el2, x0
	adr	x0, .Lin_el1
	msr	elr_el2, x0
	eret

.Lin_el1:
	/* Stack already in sp_el1 (we eret'd from EL2); also load into
	 * our current SP so plain C code uses the same memory. */
	adrp	x0, stack_top
	add	x0, x0, :lo12:stack_top
	mov	sp, x0

	/* Zero the BSS.  __bss_start / __bss_end are linker-script
	 * symbols (see uts/virt/linker.ld).  adrp + add same as the
	 * stack-pointer load above. */
	adrp	x0, __bss_start
	add	x0, x0, :lo12:__bss_start
	adrp	x1, __bss_end
	add	x1, x1, :lo12:__bss_end
1:	cmp	x0, x1
	b.eq	2f
	str	xzr, [x0], #8
	b	1b
2:
	bl	kmain

	/* kmain should never return.  If it does, halt. */
3:	wfi
	b	3b

/* ---- BSS-allocated kernel stack ------------------------------------ */
/* 64 KB single-core stack -- plenty for early init.  When SMP comes
 * online we'll move to per-CPU stacks just like the raspi3b path. */
	.section ".bss"
	.balign 16
	.global stack_bottom
	.global stack_top
stack_bottom:
	.skip 0x10000
stack_top:

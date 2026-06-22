/*
 * uts/virt/boot.S -- QEMU `virt` / AWS Graviton entry point
 *
 * Same image now boots via two paths:
 *
 *   - QEMU `-kernel build/kernel.img` (development): the Linux ARM64
 *     Image protocol.  `_start`'s first instruction is `add x13, ...`
 *     which the assembler encodes as `4d 5a 00 91` -- "MZ\x00\x91".
 *     QEMU sees `add`, executes it as a harmless no-op, then `b
 *     .Lreal_start` skips the rest of the header.
 *
 *   - UEFI (AAVMF in QEMU, AWS Nitro EC2): PE32+ EFI Application.
 *     EFI reads the "MZ" magic at offset 0, follows e_lfanew (byte 60
 *     of the DOS header) to the PE signature, parses the COFF +
 *     optional headers, loads the section data to ImageBase, jumps
 *     to AddressOfEntryPoint = efi_pe_entry.  We stash x0/x1 (image
 *     handle, system table), call efi_main (C), and on return jump
 *     to the regular `.Lreal_start` post-ExitBootServices.
 *
 * Layout (offsets in the produced kernel.img):
 *
 *   0x000  _start          ARM64 Image header (64 bytes) -- doubles
 *                          as the PE DOS header; byte 60 is e_lfanew
 *                          and points at 0x040.
 *   0x040  pe_header       PE signature + COFF header + PE32+
 *                          optional header + section table.
 *                          Sized to fit in the first 4 KB so
 *                          SizeOfHeaders=0x1000 is valid.
 *   0x1000 .text           Real code (where `-kernel` jumps into
 *                          .text.boot first).
 *
 * The hairy part: aarch64 UEFI on AAVMF / AWS Nitro hands us off
 * with MMU enabled in identity mode.  Our `.Lreal_start` does
 * `mmu_init` which turns the MMU off, reconfigures, then turns it
 * back on.  As long as identity covers our load address (it does
 * for any 1 GB-aligned RAM placement on virt/Graviton) this works
 * out -- a cleaner approach is on the stage C+ TODO list.
 */

.section ".text.boot"
.global _start

/* ---- 64-byte Linux ARM64 Image header + DOS header overlap ---- */

_start:
	add	x13, x18, #0x16		/* code0: "MZ\x00\x91" -- DOS magic */
	b	.Lreal_start		/* code1: skip past header */
	.quad	0x80000			/* text_offset = 0x80000 */
	.long	_kernel_image_size_lo32	/* image_size (lo32) */
	.long	_kernel_image_size_hi32	/* image_size (hi32) */
	.quad	0x0A			/* flags: 4K pages, anywhere */
	.quad	0
	.quad	0
	.quad	0
	.ascii	"ARM\x64"		/* ARM64 Image magic at 0x38 */
	.long	pe_header - _start	/* e_lfanew / PE offset at 0x3C */

/* ---- PE32+ EFI Application header ---- */

	.align 3
pe_header:
	.long	0x00004550			/* "PE\0\0" signature */

coff_header:
	.short	0xAA64				/* Machine: IMAGE_FILE_MACHINE_ARM64 */
	.short	(section_table_end - section_table) / 40 /* NumberOfSections */
	.long	0				/* TimeDateStamp */
	.long	0				/* PointerToSymbolTable */
	.long	0				/* NumberOfSymbols */
	.short	section_table - optional_header	/* SizeOfOptionalHeader */
	.short	0x0207				/* Characteristics:
						 *   IMAGE_FILE_RELOCS_STRIPPED (0x0001)
						 * + IMAGE_FILE_EXECUTABLE_IMAGE (0x0002)
						 * + IMAGE_FILE_LINE_NUMS_STRIPPED (0x0004)
						 * + IMAGE_FILE_DEBUG_STRIPPED (0x0200)
						 *
						 * RELOCS_STRIPPED: we have no .reloc
						 * section and ImageBase=0 only works
						 * because EFI maps us at any addr and
						 * our early code is PC-relative.  Bit
						 * tells the loader not to look. */

optional_header:
	.short	0x020B				/* Magic: PE32+ */
	.byte	0x02				/* MajorLinkerVersion */
	.byte	0x14				/* MinorLinkerVersion */
	.long	_text_section_size_lo32		/* SizeOfCode (= .text size) */
	.long	0				/* SizeOfInitializedData */
	.long	0				/* SizeOfUninitializedData */
	.long	efi_pe_entry - _start		/* AddressOfEntryPoint (RVA) */
	.long	_text_rva			/* BaseOfCode (RVA of .text) */

extra_header_fields:
	.quad	0x40080000			/* ImageBase = our linker's fixed
						 * load address.  Combined with
						 * RELOCS_STRIPPED above, EFI must
						 * load us here verbatim; if firmware
						 * insists on relocating elsewhere it
						 * fails the load.  Works on QEMU virt
						 * and AWS Graviton because RAM there
						 * starts at 0x40000000 and 0x80000
						 * is free. */
	.long	0x1000				/* SectionAlignment (4 KB) */
	.long	0x0200				/* FileAlignment (512 B) */
	.short	0				/* MajorOperatingSystemVersion */
	.short	0				/* MinorOperatingSystemVersion */
	.short	0				/* MajorImageVersion */
	.short	0				/* MinorImageVersion */
	.short	0				/* MajorSubsystemVersion */
	.short	0				/* MinorSubsystemVersion */
	.long	0				/* Win32VersionValue */
	.long	_kernel_image_size_lo32		/* SizeOfImage */
	.long	_pe_size_of_headers		/* SizeOfHeaders */
	.long	0				/* CheckSum */
	.short	0x0A				/* Subsystem: EFI_APPLICATION */
	.short	0				/* DllCharacteristics */
	.quad	0x10000				/* SizeOfStackReserve */
	.quad	0x10000				/* SizeOfStackCommit */
	.quad	0x10000				/* SizeOfHeapReserve */
	.quad	0x10000				/* SizeOfHeapCommit */
	.long	0				/* LoaderFlags */
	.long	16				/* NumberOfRvaAndSizes -- spec
						 * allows fewer but EDK II's PE
						 * loader requires the full set. */

	/* DataDirectory[16] -- all zero; EFI doesn't read these for an
	 * EFI app with no exports/imports/relocs. */
	.quad	0	/* Export Table */
	.quad	0	/* Import Table */
	.quad	0	/* Resource Table */
	.quad	0	/* Exception Table */
	.quad	0	/* Certificate Table */
	.quad	0	/* Base Relocation Table */
	.quad	0	/* Debug */
	.quad	0	/* Architecture */
	.quad	0	/* Global Ptr */
	.quad	0	/* TLS Table */
	.quad	0	/* Load Config Table */
	.quad	0	/* Bound Import */
	.quad	0	/* IAT */
	.quad	0	/* Delay Import Descriptor */
	.quad	0	/* CLR Runtime Header */
	.quad	0	/* Reserved */

section_table:
	/* .text -- covers everything past the PE headers (file offset
	 * 0x1000 onward).  Pre-0x1000 bytes are the headers themselves,
	 * which UEFI maps separately via SizeOfHeaders.  EFI maps the
	 * section as RX; the kernel's BSS lives in this section too,
	 * which means we technically declare BSS as RX -- harmless since
	 * by the time anything writes to BSS we've already brought up
	 * our own MMU tables. */
	.ascii	".text\0\0\0"			/* Name (8 bytes) */
	.long	_text_section_size_lo32		/* VirtualSize */
	.long	_text_rva			/* VirtualAddress (= 0x1000) */
	.long	_text_section_size_lo32		/* SizeOfRawData */
	.long	_text_rva			/* PointerToRawData (= 0x1000) */
	.long	0				/* PointerToRelocations */
	.long	0				/* PointerToLinenumbers */
	.short	0				/* NumberOfRelocations */
	.short	0				/* NumberOfLineNumbers */
	.long	0x60000020			/* Characteristics:
						 *   IMAGE_SCN_CNT_CODE (0x00000020)
						 * + IMAGE_SCN_MEM_EXECUTE (0x20000000)
						 * + IMAGE_SCN_MEM_READ (0x40000000) */
section_table_end:

	/* The PE header must occupy at most SizeOfHeaders bytes, and the
	 * single .text section starts at PointerToRawData = SizeOfHeaders.
	 * UEFI requires section.VirtualAddress >= SizeOfHeaders so the
	 * loader doesn't smear the section over the header region; pad
	 * to a 4 KB boundary so the section lands at the SectionAlignment
	 * line. */
	.balign 0x1000

/* ---- end of PE headers; .text section content starts here ----------
 *
 * Everything below is bytes 0x1000+ of the kernel image, which is
 * what we declared as the PE .text section.  efi_pe_entry's RVA
 * (= 0x1000 - 0 = 0x1000+offset) sits in this section, satisfying
 * the PE spec rule that entry must be inside a section. */

/* ---- EFI entry stub.  Called by UEFI with x0=image_handle and
 * x1=system_table_ptr.  Stack pointer is EFI's; we keep it.  Set up
 * x29/x30 minimally, call efi_main, on return either jump into the
 * Linux Image entry (`.Lreal_start`) or halt on failure. */
.global efi_pe_entry
efi_pe_entry:
	/* Save EFI's image handle + system table in callee-saved regs
	 * across the C call. */
	mov	x19, x0
	mov	x20, x1

	/* Switch to our kernel stack.  efi_main only needs a small
	 * stack and we're guaranteed not to clobber EFI's because we
	 * never return to the EFI loader. */
	adrp	x0, stack_top
	add	x0, x0, :lo12:stack_top
	mov	sp, x0

	/* Call efi_main(handle, system_table). */
	mov	x0, x19
	mov	x1, x20
	bl	efi_main

	/* On success, fall through to the regular kernel boot path.
	 * x0 holds efi_main's return value (EFI_SUCCESS = 0). */
	cbnz	x0, .Lefi_halt
	b	.Lreal_start
.Lefi_halt:
	wfi
	b	.Lefi_halt

/* ---- existing -kernel boot path ---- */

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

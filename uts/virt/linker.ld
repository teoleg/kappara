/*
 * uts/virt/linker.ld -- QEMU `virt` machine kernel image layout
 *
 * Memory layout differs from Pi 3:
 *
 *   0x00000000   flash / boot ROM (read-only, ignored)
 *   0x08000000   peripherals (GIC, PL011, virtio-mmio)
 *   0x40000000   start of usable RAM
 *
 * QEMU `-kernel` on the virt machine loads the image at 0x40080000 (=
 * RAM_BASE + 0x80000) by default, mirroring the Pi 3 convention.  That
 * gives us 512 KB of scratch below the kernel for early boot
 * structures and matches the offset hardcoded into `-bios` paths.
 *
 * Same kallsyms-after-BSS shape as the Pi 3 linker so the two-pass
 * link works identically.
 */

ENTRY(_start)

SECTIONS
{
	. = 0x40080000;
	__kernel_start = .;

	.text : {
		KEEP(*(.text.boot))
		*(.text*)
	}

	.rodata : ALIGN(8) {
		*(.rodata*)
	}

	.data : ALIGN(8) {
		*(.data*)
	}

	. = ALIGN(8);
	__bss_start = .;
	.bss : {
		*(.bss*)
		*(COMMON)
	}
	. = ALIGN(8);
	__bss_end = .;

	.kallsyms : ALIGN(8) {
		*(.kallsyms)
	}

	. = ALIGN(4096);
	__kernel_end = .;

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

/* AWS.md stage A: split image_size into 32-bit halves for the
 * little-endian quad field in boot.S's Linux Image header.  The
 * assembler can't compute __kernel_end - __kernel_start, but ABSOLUTE
 * here turns the expression into a symbol whose VALUE is the result --
 * boot.S then emits two .long entries that the linker resolves into
 * the right 64-bit value. */
_kernel_image_size_lo32 = ABSOLUTE((__kernel_end - __kernel_start) & 0xffffffff);
_kernel_image_size_hi32 = ABSOLUTE(((__kernel_end - __kernel_start) >> 32) & 0xffffffff);

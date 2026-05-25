/*
 * arch/arm/linker.ld -- ARMv7 kernel image layout
 *
 * QEMU `-M virt` puts RAM at 0x40000000 and loads -kernel at
 * 0x40010000.  Layout otherwise mirrors arch/aarch64/linker.ld:
 * text, rodata, data, bss; symbols __kernel_start / __bss_start /
 * __bss_end / __kernel_end are exported for boot.S to zero BSS and
 * for pmm to discover the free-RAM range.
 *
 * Extra discards on ARMv7
 * -----------------------
 *   .ARM.attributes  - architectural tags ld inserts by default
 *   .ARM.exidx*      - exception-handling tables for C++ unwind we
 *                       don't use; if left in, the linker complains
 *                       about output sections being needed.
 */

ENTRY(_start)

SECTIONS
{
	. = 0x40010000;
	__kernel_start = .;

	.text : {
		KEEP(*(.text.boot))
		*(.text*)
	}

	.rodata : ALIGN(4) {
		*(.rodata*)
	}

	.data : ALIGN(4) {
		*(.data*)
	}

	. = ALIGN(4);
	__bss_start = .;
	.bss : {
		*(.bss*)
		*(COMMON)
	}
	. = ALIGN(4);
	__bss_end = .;

	. = ALIGN(4096);
	__kernel_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
		*(.ARM.attributes)
		*(.ARM.exidx*)
	}
}

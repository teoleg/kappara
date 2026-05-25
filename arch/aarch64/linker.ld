ENTRY(_start)

SECTIONS
{
	. = 0x80000;
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

	. = ALIGN(4096);
	__kernel_end = .;

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

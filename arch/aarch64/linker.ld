ENTRY(_start)

SECTIONS
{
	. = 0x80000;

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

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

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

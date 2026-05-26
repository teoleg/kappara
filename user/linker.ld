/*
 * user/linker.ld -- link the user init binary at AArch64 EL0 VA
 *
 * The kernel publishes a 2 MB region at virtual address 0x10000000
 * with EL0-RW permissions (see USER_VA in kernel/user.c).  We link
 * the user program there so global addresses (string literals, the
 * stack frame buffer, ...) come out as constants pointing inside
 * that user-visible window.
 *
 * .text._start is forced first so _start sits at offset 0 -- the
 * kernel jumps straight to USER_VA when entering EL0 and expects
 * the user program's entry there.
 */

ENTRY(_start)

SECTIONS
{
	. = 0x10000000;

	.text : {
		KEEP(*(.text._start))
		*(.text*)
	}

	.rodata : ALIGN(8) { *(.rodata*) }
	.data   : ALIGN(8) { *(.data*) }

	. = ALIGN(8);
	.bss : { *(.bss*) *(COMMON) }

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

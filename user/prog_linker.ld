/*
 * user/prog_linker.ld -- linker script for standalone ELF user programs
 *
 * Programs built with this script load at EXEC_VA (0x20000000), which is
 * the exec address space set up by exec_space_init() in kernel/user.c.
 * The stack lives in the 2 MB above that region (EXEC_STACK_VA), so the
 * initial SP is 0x20400000 (top of the stack window).
 *
 * _start must be first so e_entry == 0x20000000 without relying on the
 * linker to discover it from the ELF symbol.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x20000000;

    .text : {
        *(.text._start)
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(8);
    .data   : { *(.data .data.*) }

    . = ALIGN(8);
    .bss    : {
        *(.bss .bss.*)
        *(COMMON)
    }
}

/*
 * lib/ld-kappara/linker.ld -- linker script for /lib/ld-kappara.so
 *
 * ld-kappara.so is ET_EXEC at a fixed VA (LD_VA = 0x30000000) --
 * deliberately NOT PIE.  It's the bootstrap loader; if it had to
 * relocate itself before it could run, we'd have a chicken-and-egg
 * problem.  Keeping it ET_EXEC means the kernel just memcpy's
 * PT_LOAD into LD_VA and jumps -- no fixup walk required.
 *
 * The file is small (a handful of instructions for the stage 5
 * pass-through; will grow when stage 6 adds DT_NEEDED handling).
 * Reserve a 1 MB window at LD_VA for now.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x30000000;

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

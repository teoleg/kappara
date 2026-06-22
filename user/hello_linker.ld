/*
 * user/hello_linker.ld -- linker script for the standalone hello.elf
 *
 * /bin/hello stays ET_EXEC (no libc, no relocations) -- it's the
 * minimal test path that exercises the loader's ET_EXEC branch
 * after cmd/* moved to PIE in docs/DYNAMIC.md stages 3+4.
 *
 * Hard-codes the load VA at EXEC_VA so the loader doesn't have to
 * relocate it; matches the contract sys_execve_impl uses when
 * load_base = 0 for ET_EXEC.
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

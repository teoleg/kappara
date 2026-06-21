/*
 * user/prog_linker.ld -- linker script for PIE cmd binaries
 *
 * Stage 3+4 of docs/DYNAMIC.md: cmd binaries are now ET_DYN (static
 * PIE).  The linker emits p_vaddr values relative to 0; kappara's
 * exec loader picks a load_base (EXEC_VA = 0x20000000) at runtime
 * and applies R_AARCH64_RELATIVE relocations from .rela.dyn.
 *
 * _start sits in the first PT_LOAD at offset SIZEOF_HEADERS, so
 * e_entry is small (typically 0x238 once the headers fit).  The
 * kernel jumps to load_base + e_entry.
 *
 * PHDRS is explicit because the default ld template for -pie emits
 * a PT_PHDR header that has to be covered by the first PT_LOAD --
 * leaving it implicit produces "PHDR segment not covered by LOAD".
 */
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FILEHDR PHDRS FLAGS(5);   /* R-X */
    data    PT_LOAD    FLAGS(6);                 /* RW- */
    dynamic PT_DYNAMIC FLAGS(4);
}

SECTIONS
{
    . = SIZEOF_HEADERS;

    .hash    : { *(.hash) }                                :text
    .gnu.hash : { *(.gnu.hash) }                           :text
    .dynsym  : { *(.dynsym) }                              :text
    .dynstr  : { *(.dynstr) }                              :text
    .rela.dyn : {
        *(.rela.init)
        *(.rela.text .rela.text.* .rela.gnu.linkonce.t.*)
        *(.rela.fini)
        *(.rela.rodata .rela.rodata.* .rela.gnu.linkonce.r.*)
        *(.rela.data .rela.data.* .rela.gnu.linkonce.d.*)
        *(.rela.ctors)
        *(.rela.dtors)
        *(.rela.got)
        *(.rela.bss .rela.bss.* .rela.gnu.linkonce.b.*)
    }                                                      :text
    .rela.plt : { *(.rela.plt) }                           :text

    .text : {
        *(.text._start)
        *(.text .text.*)
    }                                                      :text

    .rodata : { *(.rodata .rodata.*) }                     :text

    . = ALIGN(4096);

    .dynamic : { *(.dynamic) }                             :data :dynamic
    .got     : { *(.got) *(.got.plt) }                     :data

    . = ALIGN(8);
    .data    : { *(.data .data.*) }                        :data

    . = ALIGN(8);
    .bss     : {
        *(.bss .bss.*)
        *(COMMON)
    }                                                      :data

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.gnu_debuglink) }
}

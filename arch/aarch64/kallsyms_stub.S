/*
 * arch/aarch64/kallsyms_stub.S -- pass-1 placeholder for kallsyms
 *
 * The two-pass build needs SOMETHING to define the __kallsyms_*
 * symbols so the pass-1 link succeeds and we can nm the result.
 * This stub emits an empty table (count=0, no entries, no strings)
 * so ksym_lookup gracefully returns NULL on pass 1.
 *
 * On pass 2 this object is replaced by the real, populated table
 * emitted by tools/gen_kallsyms.sh.  Because .kallsyms is placed
 * after .bss in the linker script, growing it across passes does
 * not shift any code or data symbol addresses.
 */

	.section .kallsyms, "a"
	.align 8

	.globl __kallsyms_magic
__kallsyms_magic:
	.4byte 0			/* magic 0 -> "not populated" */

	.globl __kallsyms_count
__kallsyms_count:
	.4byte 0

	.globl __kallsyms_table
__kallsyms_table:

	.globl __kallsyms_strings
__kallsyms_strings:
	.byte 0

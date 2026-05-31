/*
 * arch/aarch64/linker.ld -- AArch64 kernel image layout
 * =====================================================
 *
 * Tells the linker where to put each section of the linked kernel
 * image.  The image is then `objcopy -O binary` stripped to a raw
 * blob (kernel8.img) and loaded by QEMU's -kernel option at 0x80000,
 * which is where the Pi 3 firmware would also drop us on real hardware.
 *
 * Memory layout
 * -------------
 *
 *   0x00000000   +------------------+
 *                | scratch space:   |  used as boot stack (grows down
 *                |  initial stack,  |  from 0x80000), then for early
 *                |  device tree,    |  page tables / heap once mmu_init
 *                |  ...             |  has run and pmm has the rest.
 *                |                  |
 *   0x00080000   +------------------+ <-- __kernel_start, _start
 *                | .text            |   boot code + kernel
 *                |   (.text.boot    |
 *                |    forced first  |   KEEP() so the linker can't
 *                |    by the script)|   garbage-collect _start
 *                +------------------+
 *                | .rodata          |   string literals, vec_name[], ...
 *                +------------------+
 *                | .data            |   initialised globals
 *                +------------------+
 *                | .bss             |   zero-initialised globals
 *                |   (page tables,  |   l0/l1/l2_table live here
 *                |    size_caches,  |
 *                |    main_thread)  |
 *                +------------------+ <-- __kernel_end (page aligned)
 *                | free RAM         |   handed to pmm by pmm_init
 *                | ...              |
 *   0x3F000000   +------------------+
 *                | BCM2837          |   PL011 UART at 0x3F201000
 *                | peripherals      |   mapped Device-nGnRE by mmu_init
 *   0x40000000   +------------------+
 *                | ARM local        |   per-core timer routing /
 *                | peripherals      |   IRQ source at 0x40000040..
 *                +------------------+
 *
 * Symbols exported to C
 * ---------------------
 *   __kernel_start  start of the kernel image
 *   __bss_start     first byte of BSS (used by boot.S to zero it)
 *   __bss_end       one-past-last byte of BSS
 *   __kernel_end    page-aligned end of everything; pmm_init starts here
 */

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

	/* Kernel symbol table for backtrace.  Placed after BSS so its
	 * size doesn't shift any code/data addresses between the two
	 * link passes -- see tools/gen_kallsyms.sh. */
	.kallsyms : ALIGN(8) {
		*(.kallsyms)
	}

	. = ALIGN(4096);
	__kernel_end = .;

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

/*
 * include/kappara/arch/efi.h -- AWS.md stage B runtime globals.
 *
 * efi_main (uts/virt/efi_main.c) is what runs when EDK II hands
 * the kernel control through the PE32+ entry point.  It snapshots
 * a small amount of firmware state into BSS before
 * ExitBootServices, then returns; the rest of the kernel consumes
 * that snapshot through the symbols declared here.
 *
 * Under the QEMU `-kernel` boot path the globals stay NULL/zero;
 * callers must handle that (e.g. /proc/efi prints "no memory map",
 * the ACPI parser logs "no RSDP and returns).
 *
 * The full uts/virt/efi.h header holds the internal UEFI Boot
 * Services + System Table types that efi_main needs to call into
 * the firmware -- nothing outside the EFI entry path needs those.
 */
#ifndef KAPPARA_ARCH_EFI_H
#define KAPPARA_ARCH_EFI_H

#include <stdint.h>

/* EFI memory descriptor (UEFI spec §7.2 GetMemoryMap).  The
 * structure layout is fixed; firmware's "descriptor_size" may be
 * larger than sizeof(this) because future fields are appended.
 * Callers iterate with that stride, NOT sizeof. */
struct efi_memory_descriptor {
	uint32_t type;
	uint32_t pad;
	uint64_t physical_start;
	uint64_t virtual_start;
	uint64_t number_of_pages;
	uint64_t attribute;
};

/* What efi_main found. */
extern void                         *efi_acpi_rsdp;	  /* ACPI 2.0 RSDP */
extern struct efi_memory_descriptor *efi_memmap_buf;
extern uint64_t                      efi_memmap_size;
extern uint64_t                      efi_memmap_descriptor_size;
extern uint64_t                      efi_memmap_descriptor_version;

#endif /* KAPPARA_ARCH_EFI_H */

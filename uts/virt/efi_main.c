/*
 * uts/virt/efi_main.c -- AWS.md stage B: UEFI entry point.
 *
 * Called by UEFI firmware (AAVMF in QEMU, AWS Nitro UEFI on EC2)
 * via the PE32+ optional header's AddressOfEntryPoint, which boot.S
 * sets to `efi_pe_entry`.  efi_pe_entry stashes x0/x1 (image handle
 * and system table) and tail-calls this C function.
 *
 * What this does today:
 *
 *   1. Walks system_table->tables (the EFI Configuration Table) for
 *      the ACPI 2.0 RSDP, stashing the pointer in a static so the
 *      kernel can read it after MMU bring-up.
 *   2. Snapshots the EFI memory map (size + descriptors), saving
 *      the buffer in BSS so stage C+ can drive the pmm enrolment off
 *      it instead of the hardcoded virt MMIO map.
 *   3. Calls ExitBootServices.  After this point all the function
 *      pointers in `bs` and `tables` are dead -- we own the machine.
 *   4. Returns control to boot.S, which jumps to the normal kernel
 *      startup path (`.Lreal_start`).
 *
 * What this DOESN'T do yet:
 *
 *   - Disable the EFI MMU mapping.  EFI hands us off with the MMU
 *     enabled in identity-map mode; our mmu_init() turns it off,
 *     reconfigures, turns it back on with our own tables.  As long
 *     as identity mapping covers our physical address that works,
 *     but it's fragile -- stage C does the cleanup.
 *   - Anything device-specific.  No console writes, no probe.  The
 *     first sign of life under UEFI is whatever kmain prints over
 *     the PL011, same as the -kernel path.
 */

#include <stdint.h>

#include "efi.h"

/* Filled in by efi_main, read by kmain.  The kernel proper hasn't
 * started yet when these are written, so they sit in plain BSS;
 * once stage C wires up ACPI parsing the global variables here are
 * what it walks. */
void                     *efi_acpi_rsdp;
efi_memory_descriptor_t  *efi_memmap_buf;
uint64_t                  efi_memmap_size;
uint64_t                  efi_memmap_descriptor_size;
uint64_t                  efi_memmap_descriptor_version;

/* Static scratch buffer for the memory map.  4 KB holds about 80
 * descriptors at the EFI 48-byte size; AAVMF + virt produces 20-40
 * so this is comfortable. */
static uint8_t            efi_memmap_storage[4096];

static int guid_eq(const efi_guid_t *a, const efi_guid_t *b)
{
	if (a->data1 != b->data1 || a->data2 != b->data2 || a->data3 != b->data3)
		return 0;
	for (unsigned i = 0; i < 8; i++)
		if (a->data4[i] != b->data4[i]) return 0;
	return 1;
}

/* Returned to boot.S; if zero, halt -- we can't continue safely. */
efi_status_t efi_main(efi_handle_t image_handle, efi_system_table_t *st)
{
	if (!st || !st->boot_services) return 1;

	/* 1: walk the configuration table for ACPI 2.0. */
	const efi_guid_t acpi_guid = EFI_ACPI_20_TABLE_GUID;
	for (efi_uintn_t i = 0; i < st->nr_tables; i++) {
		if (guid_eq(&st->tables[i].vendor_guid, &acpi_guid)) {
			efi_acpi_rsdp = st->tables[i].vendor_table;
			break;
		}
	}

	/* 2: snapshot the memory map.  GetMemoryMap is called twice in
	 * the typical UEFI dance -- first to learn the size, then to
	 * fetch the data + the key required by ExitBootServices.  Our
	 * static buffer is sized once and for all so the first call is
	 * skipped; if the firmware returns BUFFER_TOO_SMALL we bail. */
	efi_uintn_t  size = sizeof(efi_memmap_storage);
	efi_uintn_t  map_key = 0;
	efi_uintn_t  desc_size = 0;
	uint32_t     desc_version = 0;
	efi_status_t s = st->boot_services->get_memory_map(
				&size,
				efi_memmap_storage,
				&map_key,
				&desc_size,
				&desc_version);
	if (s != EFI_SUCCESS) return s;

	efi_memmap_buf                = (efi_memory_descriptor_t *)efi_memmap_storage;
	efi_memmap_size               = size;
	efi_memmap_descriptor_size    = desc_size;
	efi_memmap_descriptor_version = desc_version;

	/* 3: exit boot services.  After this returns successfully, the
	 * boot-services dispatch table is invalid; we own everything. */
	s = st->boot_services->exit_boot_services(image_handle, map_key);
	if (s != EFI_SUCCESS) {
		/* Spec allows EFI to invalidate the map between get + exit;
		 * a real loader retries with a refreshed map.  For stage B
		 * we just give up. */
		return s;
	}

	return EFI_SUCCESS;
}

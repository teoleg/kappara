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
#include "platform.h"

/* Filled in by efi_main, read by kmain.  The kernel proper hasn't
 * started yet when these are written, so they sit in plain BSS;
 * once stage C wires up ACPI parsing the global variables here are
 * what it walks. */
void                     *efi_acpi_rsdp;
efi_memory_descriptor_t  *efi_memmap_buf;
uint64_t                  efi_memmap_size;
uint64_t                  efi_memmap_descriptor_size;
uint64_t                  efi_memmap_descriptor_version;
/* UART base from ACPI SPCR.  Zero on the -kernel path (efi_main
 * never runs); uart_init() falls back to PLAT_PL011_BASE in that
 * case.  mmu_init() uses this to wire the UART region before PMM
 * is available. */
uint64_t                  efi_uart_base;
/* SPCR interface type (ACPI spec §18.3.3):
 *   0x00 = 16550-compatible
 *   0x03 = ARM PL011
 *   0x05 = ARM SBSA Generic UART
 * Defaults to 3 (PL011) for the -kernel path where efi_main never
 * runs and PLAT_PL011_BASE is a real PL011. */
uint8_t                   efi_uart_type = 3;
/* Baud-rate divisors — meaningful only for PL011/SBSA (type 3/5).
 * Saved before ExitBootServices so uart_init() can restore them if
 * the firmware clears them during EBS cleanup. */
uint32_t                  efi_uart_ibrd;
uint32_t                  efi_uart_fbrd;

/* Static scratch buffer for the memory map.  AAVMF + virt produces
 * 20-40 descriptors; AWS Nitro produces 100-150+ (NVMe, ENA, PCIe
 * BARs, EFI runtime code/data, ACPI tables, etc.).  32 KB handles
 * ~680 descriptors at the UEFI 2.x default 48-byte size, which is
 * enough for any realistic Graviton instance.  BSS is zero-filled by
 * the EFI loader (the PE section is RWX + SizeOfImage-padded), so
 * the extra size costs nothing on disk. */
static uint8_t            efi_memmap_storage[32768];

/* Walk RSDP -> XSDT -> find "SPCR" -> return UART base from GAS.
 * SPCR layout (ACPI 6.x): standard 36-byte header, then 1-byte
 * interface_type, 3 reserved bytes, then a 12-byte Generic Address
 * Structure whose 8-byte Address field sits at table offset 44.
 * Called before ExitBootServices while ACPI table pages are mapped. */
static uint64_t spcr_find_uart(void *rsdp_ptr)
{
	if (!rsdp_ptr) return 0;
	uint8_t *rsdp = (uint8_t *)rsdp_ptr;
	if (rsdp[15] < 2) return 0;	/* need ACPI 2.0 for XSDT */

	uint64_t xsdt_pa = 0;
	for (int i = 0; i < 8; i++)
		xsdt_pa |= (uint64_t)rsdp[24 + i] << (i * 8);
	if (!xsdt_pa) return 0;

	uint8_t *xsdt = (uint8_t *)(uintptr_t)xsdt_pa;
	uint32_t xlen = (uint32_t)xsdt[4] | ((uint32_t)xsdt[5] << 8) |
	                ((uint32_t)xsdt[6] << 16) | ((uint32_t)xsdt[7] << 24);
	if (xlen < 36) return 0;

	unsigned n = (xlen - 36) / 8;
	for (unsigned i = 0; i < n; i++) {
		uint64_t entry = 0;
		uint8_t *ep = xsdt + 36 + i * 8;
		for (int j = 0; j < 8; j++)
			entry |= (uint64_t)ep[j] << (j * 8);
		if (!entry) continue;
		uint8_t *tbl = (uint8_t *)(uintptr_t)entry;
		if (tbl[0]=='S' && tbl[1]=='P' && tbl[2]=='C' && tbl[3]=='R') {
			/* tbl[36] = InterfaceType (ACPI SPCR spec byte 36,
			 * immediately after the 36-byte table header). */
			extern uint8_t efi_uart_type;
			efi_uart_type = tbl[36];
			uint64_t base = 0;
			for (int j = 0; j < 8; j++)
				base |= (uint64_t)tbl[44 + j] << (j * 8);
			return base;
		}
	}
	return 0;
}

/* Print a short ASCII string via EFI's ConOut (before ExitBootServices).
 * Converts ASCII to the UCS-2 ConOut expects, 96 chars max per call.
 * Called with con_out != NULL only. */
static void efi_print(efi_simple_text_output_protocol_t *con, const char *s)
{
	efi_char16_t buf[97];
	unsigned i;
	for (i = 0; i < 96 && s[i]; i++)
		buf[i] = (efi_char16_t)(unsigned char)s[i];
	buf[i] = 0;
	con->output_string(con, buf);
}

static void efi_print_hex(efi_simple_text_output_protocol_t *con, uint64_t v)
{
	char s[19];
	s[0] = '0'; s[1] = 'x';
	for (int i = 0; i < 16; i++) {
		int n = (int)((v >> (60 - i * 4)) & 0xF);
		s[2 + i] = (char)(n < 10 ? '0' + n : 'a' + n - 10);
	}
	s[18] = 0;
	efi_print(con, s);
}

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

#define PRINT(msg) do { if (st->con_out) efi_print(st->con_out, msg); } while(0)

	PRINT("kappara: efi_main\r\n");

	/* 1: walk the configuration table for ACPI 2.0. */
	const efi_guid_t acpi_guid = EFI_ACPI_20_TABLE_GUID;
	for (efi_uintn_t i = 0; i < st->nr_tables; i++) {
		if (guid_eq(&st->tables[i].vendor_guid, &acpi_guid)) {
			efi_acpi_rsdp = st->tables[i].vendor_table;
			break;
		}
	}

	/* Extract UART base from SPCR while ACPI pages are still mapped.
	 * Must happen before ExitBootServices; mmu_init() reads this to
	 * wire the UART region without needing PMM. */
	efi_uart_base = spcr_find_uart(efi_acpi_rsdp);
	if (efi_uart_base) {
		/* Trust the SPCR address and interface type without probing.
		 * Previous code probed offset 0x18 assuming a PL011 FR register,
		 * but Nitro's SPCR points to a 16550-compatible device where
		 * that offset is meaningless — the probe always returned
		 * 0xFFFFFFFF and we incorrectly discarded the correct address.
		 * The SPCR InterfaceType field tells us the driver to use. */
		PRINT("kappara: SPCR UART=");
		if (st->con_out) efi_print_hex(st->con_out, efi_uart_base);
		PRINT(" type=");
		if (st->con_out) efi_print_hex(st->con_out, (uint64_t)efi_uart_type);
		PRINT("\r\n");
	} else {
		PRINT("kappara: no SPCR, using platform default\r\n");
		efi_uart_base = PLAT_PL011_BASE;
		efi_uart_type = 3; /* PL011 */
	}

	/* Save PL011/SBSA baud-rate divisors before ExitBootServices.
	 * Irrelevant for 16550 (which uses different divisor registers and
	 * whose baud rate is managed by the Nitro hypervisor anyway). */
	if (efi_uart_type == 3 || efi_uart_type == 5) {
		efi_uart_ibrd = *(volatile uint32_t *)(uintptr_t)(efi_uart_base + 0x24);
		efi_uart_fbrd = *(volatile uint32_t *)(uintptr_t)(efi_uart_base + 0x28);
		PRINT("kappara: PL011 IBRD=");
		if (st->con_out) efi_print_hex(st->con_out, efi_uart_ibrd);
		PRINT(" FBRD=");
		if (st->con_out) efi_print_hex(st->con_out, efi_uart_fbrd);
		PRINT("\r\n");
	}

	/* 2: snapshot the memory map.  GetMemoryMap is called twice in
	 * the typical UEFI dance: first to get the required buffer size,
	 * then to fetch the map + key needed by ExitBootServices.  We
	 * skip the first call and use our large static buffer directly;
	 * at 32 KB it comfortably holds 680+ descriptors (Nitro typically
	 * produces 100-150). */
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
	if (s != EFI_SUCCESS) {
		PRINT("kappara: GetMemoryMap failed\r\n");
		return s;
	}

	efi_memmap_buf                = (efi_memory_descriptor_t *)efi_memmap_storage;
	efi_memmap_size               = size;
	efi_memmap_descriptor_size    = desc_size;
	efi_memmap_descriptor_version = desc_version;

	PRINT("kappara: ExitBootServices\r\n");

	/* 3: exit boot services.  The UEFI spec allows firmware to
	 * invalidate the map key between get_memory_map and
	 * exit_boot_services (e.g. the console write above allocated a
	 * pool buffer internally).  Retry once with a refreshed map if
	 * that happens -- this is the standard UEFI loader dance. */
	s = st->boot_services->exit_boot_services(image_handle, map_key);
	if (s == EFI_INVALID_PARAMETER) {
		/* Map changed under us -- refresh key and retry. */
		size = sizeof(efi_memmap_storage);
		s = st->boot_services->get_memory_map(&size,
					efi_memmap_storage, &map_key,
					&desc_size, &desc_version);
		if (s != EFI_SUCCESS) return s;
		efi_memmap_size               = size;
		efi_memmap_descriptor_size    = desc_size;
		efi_memmap_descriptor_version = desc_version;
		s = st->boot_services->exit_boot_services(image_handle, map_key);
	}
	if (s != EFI_SUCCESS)
		return s;

#undef PRINT
	return EFI_SUCCESS;
}

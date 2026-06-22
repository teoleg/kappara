/*
 * uts/virt/efi.h -- minimum UEFI types used by efi_main.
 *
 * AWS.md stage B: just enough of the UEFI spec to walk the
 * configuration table for the ACPI 2.0 entry and call
 * ExitBootServices.  All struct layouts come from the UEFI 2.10
 * spec; only the fields we actually touch are populated.  Pointer
 * fields we don't read are typed as `void *` to keep the header
 * short.
 *
 * Calling convention: aarch64 UEFI uses the SysV AAPCS64, same as
 * the rest of the kernel, so no special wrapper macros are needed
 * for the function pointers.
 */

#ifndef KAPPARA_VIRT_EFI_H
#define KAPPARA_VIRT_EFI_H

#include <stdint.h>

typedef uint64_t       efi_uintn_t;
typedef uint64_t       efi_status_t;
typedef void          *efi_handle_t;
typedef uint16_t       efi_char16_t;

#define EFI_SUCCESS                   0
#define EFI_INVALID_PARAMETER         (0x8000000000000000ULL | 2)
#define EFI_BUFFER_TOO_SMALL          (0x8000000000000000ULL | 5)

/* 128-bit GUID, byte order matches the C-format the UEFI spec uses. */
typedef struct {
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t  data4[8];
} efi_guid_t;

/* ACPI 2.0 root pointer GUID: 8868e871-e4f1-11d3-bc22-0080c73c8881.
 * Walking system_table->ConfigurationTable for this GUID finds the
 * RSDP -- our entry into the ACPI tables. */
#define EFI_ACPI_20_TABLE_GUID \
	{ 0x8868e871, 0xe4f1, 0x11d3, \
	  { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

typedef struct {
	efi_guid_t vendor_guid;
	void      *vendor_table;
} efi_configuration_table_t;

typedef struct {
	uint64_t           signature;
	uint32_t           revision;
	uint32_t           header_size;
	uint32_t           crc32;
	uint32_t           reserved;
} efi_table_header_t;

/* EFI memory descriptor, as returned by GetMemoryMap. */
typedef struct {
	uint32_t type;
	uint32_t pad;
	uint64_t physical_start;
	uint64_t virtual_start;
	uint64_t number_of_pages;
	uint64_t attribute;
} efi_memory_descriptor_t;

/* Boot Services -- a fat dispatch table of function pointers we
 * call into.  Only the entries we need are typed; the rest are
 * `void *` placeholders so the struct sizing matches the spec. */
typedef struct {
	efi_table_header_t hdr;

	void *raise_tpl;
	void *restore_tpl;

	void *allocate_pages;
	void *free_pages;

	/* GetMemoryMap(uintn_t *memory_map_size, void *memory_map,
	 *              uintn_t *map_key, uintn_t *descriptor_size,
	 *              uint32_t *descriptor_version) */
	efi_status_t (*get_memory_map)(efi_uintn_t *size, void *map,
				       efi_uintn_t *key,
				       efi_uintn_t *descriptor_size,
				       uint32_t *descriptor_version);

	void *allocate_pool;
	void *free_pool;

	void *create_event;
	void *set_timer;
	void *wait_for_event;
	void *signal_event;
	void *close_event;
	void *check_event;

	void *install_protocol_interface;
	void *reinstall_protocol_interface;
	void *uninstall_protocol_interface;
	void *handle_protocol;
	void *reserved;
	void *register_protocol_notify;
	void *locate_handle;
	void *locate_device_path;
	void *install_configuration_table;

	void *load_image;
	void *start_image;
	void *exit;
	void *unload_image;

	/* ExitBootServices(efi_handle_t image_handle, uintn_t map_key) */
	efi_status_t (*exit_boot_services)(efi_handle_t image_handle,
					   efi_uintn_t map_key);

	void *get_next_monotonic_count;
	void *stall;
	void *set_watchdog_timer;
} efi_boot_services_t;

/* Simple text output protocol -- used for the "ACPI at 0x..." message
 * we print before ExitBootServices.  After ExitBootServices we lose
 * access to this and have to fall back to direct PL011 writes. */
typedef struct efi_simple_text_output_protocol efi_simple_text_output_protocol_t;
struct efi_simple_text_output_protocol {
	void *reset;
	efi_status_t (*output_string)(efi_simple_text_output_protocol_t *self,
				      efi_char16_t *string);
	void *test_string;
	void *query_mode;
	void *set_mode;
	void *set_attribute;
	void *clear_screen;
	void *set_cursor_position;
	void *enable_cursor;
	void *mode;
};

/* System table -- the root pointer EFI hands us in x1. */
typedef struct {
	efi_table_header_t hdr;

	efi_char16_t       *firmware_vendor;
	uint32_t            firmware_revision;

	efi_handle_t        console_in_handle;
	void               *con_in;

	efi_handle_t        console_out_handle;
	efi_simple_text_output_protocol_t *con_out;

	efi_handle_t        standard_error_handle;
	efi_simple_text_output_protocol_t *std_err;

	void               *runtime_services;
	efi_boot_services_t *boot_services;

	efi_uintn_t                 nr_tables;
	efi_configuration_table_t  *tables;
} efi_system_table_t;

#endif /* KAPPARA_VIRT_EFI_H */

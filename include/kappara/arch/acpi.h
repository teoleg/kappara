/*
 * include/kappara/arch/acpi.h -- ACPI static-table layouts + discovery API.
 *
 * AWS.md stage C.  We only consume the data tables; no AML.  All
 * structs match the ACPI 6.4 spec exactly, packed to 1 byte
 * (mandatory -- ACPI's MADT sub-entries pack 8-byte addresses on
 * unaligned 4-byte boundaries and 2-byte reserveds before 4-byte
 * fields).  Field widths are explicit-size types so the layout is
 * fixed across builds.
 */
#ifndef KAPPARA_ARCH_ACPI_H
#define KAPPARA_ARCH_ACPI_H

#include <stdint.h>

/* RSDP: ACPI 2.0+ flavour.  The 1.0 form is shorter (20 bytes); we
 * only support 2.0+ because we always get the pointer from the EFI
 * configuration table's ACPI 2.0 GUID entry. */
struct acpi_rsdp {
	char     signature[8];		/* "RSD PTR " */
	uint8_t  checksum;		/* sum of first 20 bytes = 0 */
	char     oemid[6];
	uint8_t  revision;		/* 2 for ACPI 2.0+ */
	uint32_t rsdt_address;		/* 32-bit RSDT (legacy) */
	uint32_t length;		/* total RSDP length */
	uint64_t xsdt_address;		/* 64-bit XSDT (what we use) */
	uint8_t  ext_checksum;		/* sum of all `length` bytes = 0 */
	uint8_t  reserved[3];
} __attribute__((packed));

/* Standard SDT header.  All tables (XSDT, MADT, MCFG, GTDT, FADT,
 * ...) start with this 36-byte block.  `length` is the full table
 * length including this header. */
struct acpi_sdt_header {
	char     signature[4];
	uint32_t length;
	uint8_t  revision;
	uint8_t  checksum;		/* sum of `length` bytes = 0 */
	char     oem_id[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed));

/* XSDT: header followed by an array of 64-bit pointers to other
 * tables.  Number of entries = (length - sizeof(header)) / 8. */
struct acpi_xsdt {
	struct acpi_sdt_header header;
	uint64_t entries[];		/* table pointers */
} __attribute__((packed));

/* MADT (signature "APIC"): SDT header + LAPIC fields + variable
 * array of sub-entries.  On ARM the LAPIC fields are meaningless;
 * we walk the sub-entries by type. */
struct acpi_madt {
	struct acpi_sdt_header header;
	uint32_t local_int_controller_address;	/* x86 LAPIC; ignored */
	uint32_t flags;
	uint8_t  entries[];			/* variable-length array */
} __attribute__((packed));

/* MADT sub-entry types we care about (ACPI 6.4 §5.2.12). */
#define ACPI_MADT_TYPE_GICC	0x0B	/* GIC CPU interface */
#define ACPI_MADT_TYPE_GICD	0x0C	/* GIC distributor */
#define ACPI_MADT_TYPE_GIC_MSI	0x0D	/* GIC MSI frame */
#define ACPI_MADT_TYPE_GICR	0x0E	/* GIC redistributor */
#define ACPI_MADT_TYPE_GIC_ITS	0x0F	/* GIC ITS */

/* All MADT sub-entries share this 2-byte header. */
struct acpi_madt_subhdr {
	uint8_t  type;
	uint8_t  length;
} __attribute__((packed));

/* GICC: one per CPU.  arm_mpidr is what we match against MPIDR_EL1
 * to identify the boot CPU and enumerate secondaries.  Length 80. */
struct acpi_madt_gicc {
	uint8_t  type;			/* 0x0B */
	uint8_t  length;		/* 80 */
	uint16_t reserved;
	uint32_t cpu_interface_number;
	uint32_t acpi_processor_uid;
	uint32_t flags;			/* bit 0: enabled */
	uint32_t parking_protocol_version;
	uint32_t performance_interrupt_gsiv;
	uint64_t parked_address;
	uint64_t physical_base_address;	/* GICv2 only, 0 on v3 */
	uint64_t gicv;
	uint64_t gich;
	uint32_t vgic_maintenance_interrupt;
	uint64_t gicr_base_address;	/* GICv3 per-CPU redist; 0 if
					 * platform uses GICR ranges */
	uint64_t arm_mpidr;
	uint8_t  cpu_efficiency_class;
	uint8_t  reserved2;
	uint16_t spe_overflow_interrupt;
} __attribute__((packed));

/* GICD: usually exactly one.  physical_base_address is what we
 * currently hardcode as PLAT_GIC_DIST_BASE. */
struct acpi_madt_gicd {
	uint8_t  type;			/* 0x0C */
	uint8_t  length;		/* 24 */
	uint16_t reserved;
	uint32_t gic_id;
	uint64_t physical_base_address;
	uint32_t system_vector_base;
	uint8_t  gic_version;		/* 0=auto, 2=v2, 3=v3 */
	uint8_t  reserved2[3];
} __attribute__((packed));

/* GICR: a single MMIO range that contains every redistributor's
 * frame, stride 0x20000 (GICv3) or 0x40000 (GICv4). */
struct acpi_madt_gicr {
	uint8_t  type;			/* 0x0E */
	uint8_t  length;		/* 16 */
	uint16_t reserved;
	uint64_t discovery_range_base_address;
	uint32_t discovery_range_length;
} __attribute__((packed));

/* MCFG (signature "MCFG"): SDT header + 8-byte reserved + array of
 * 16-byte allocation entries.  base_address + segment combination
 * uniquely identifies a PCIe ECAM region. */
struct acpi_mcfg_alloc {
	uint64_t base_address;
	uint16_t pci_segment_group;
	uint8_t  start_bus_number;
	uint8_t  end_bus_number;
	uint32_t reserved;
} __attribute__((packed));

struct acpi_mcfg {
	struct acpi_sdt_header header;
	uint64_t reserved;
	struct acpi_mcfg_alloc allocations[];
} __attribute__((packed));

/* GTDT (signature "GTDT"): generic timer info.  We mostly care
 * about cnt_control_base (=0 on QEMU virt; CNTFRQ_EL0 sysreg is
 * the source of truth) and the non-secure EL1 GSIV. */
struct acpi_gtdt {
	struct acpi_sdt_header header;
	uint64_t cnt_control_base;
	uint32_t reserved;
	uint32_t secure_el1_timer_gsiv;
	uint32_t secure_el1_timer_flags;
	uint32_t non_secure_el1_timer_gsiv;
	uint32_t non_secure_el1_timer_flags;
	uint32_t virtual_el1_timer_gsiv;
	uint32_t virtual_el1_timer_flags;
	uint32_t el2_timer_gsiv;
	uint32_t el2_timer_flags;
	uint64_t cnt_read_base;
	uint32_t platform_timer_count;
	uint32_t platform_timer_offset;
} __attribute__((packed));

/* FADT (signature "FACP"): huge.  We only read the version + flags
 * for now; future stages may consume more.  Truncated to header +
 * first few fields here to keep the file small. */
struct acpi_fadt_min {
	struct acpi_sdt_header header;
	uint32_t firmware_ctrl;		/* 32-bit FACS, may be 0 */
	uint32_t dsdt;			/* 32-bit DSDT, may be 0 */
	uint8_t  reserved;
	uint8_t  preferred_pm_profile;
	uint16_t sci_int;
	uint32_t smi_cmd;
	uint8_t  acpi_enable;
	uint8_t  acpi_disable;
	uint8_t  s4bios_req;
	uint8_t  pstate_cnt;
	uint32_t pm1a_evt_blk, pm1b_evt_blk;
	uint32_t pm1a_cnt_blk, pm1b_cnt_blk;
	uint32_t pm2_cnt_blk, pm_tmr_blk;
	uint32_t gpe0_blk, gpe1_blk;
	uint8_t  pm1_evt_len, pm1_cnt_len, pm2_cnt_len, pm_tmr_len;
	uint8_t  gpe0_blk_len, gpe1_blk_len, gpe1_base, cst_cnt;
	uint16_t p_lvl2_lat, p_lvl3_lat;
	uint16_t flush_size, flush_stride;
	uint8_t  duty_offset, duty_width;
	uint8_t  day_alrm, mon_alrm, century;
	uint16_t iapc_boot_arch;
	uint8_t  reserved2;
	uint32_t flags;
} __attribute__((packed));

/* ---- Discovery API ---- */

/* Walk the EFI-supplied RSDP (uts/virt/efi_main.c::efi_acpi_rsdp)
 * and populate the globals below.  Safe to call with no RSDP --
 * just sets acpi_present=0 and returns.  Logs a one-line summary
 * via kprintf either way. */
void acpi_init(void);

/* What we found.  All zero/NULL if acpi_present=0 (no UEFI boot
 * or table missing).  Stage D consumes these; Stage C just prints
 * them. */
extern int       acpi_present;
extern uint64_t  acpi_gicd_base;	/* MADT GICD entry */
extern uint8_t   acpi_gic_version;	/* MADT GICD entry */
extern uint64_t  acpi_gicr_base;	/* MADT GICR entry (range) */
extern uint64_t  acpi_gicr_length;
extern uint32_t  acpi_nr_cpus;		/* count of enabled GICCs */
#define ACPI_MAX_CPUS 32
extern uint64_t  acpi_cpu_mpidr[ACPI_MAX_CPUS];
extern uint64_t  acpi_pcie_ecam_base;	/* MCFG first allocation */
extern uint8_t   acpi_pcie_bus_start;
extern uint8_t   acpi_pcie_bus_end;
extern uint32_t  acpi_timer_ns_el1_gsiv;	/* GTDT */
extern uint32_t  acpi_timer_virt_el1_gsiv;	/* GTDT */
extern uint64_t  acpi_fadt_flags;	/* FADT flags, 32 bits zero-extended */

#endif /* KAPPARA_ARCH_ACPI_H */

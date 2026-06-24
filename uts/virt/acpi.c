/*
 * uts/virt/acpi.c -- AWS.md stage C: parse the static ACPI tables.
 *
 * Driven by uts/virt/efi_main.c which captured the RSDP pointer
 * from the EFI Configuration Table (ACPI 2.0 GUID).  We walk:
 *
 *   RSDP -> XSDT -> { MADT, MCFG, GTDT, FADT }
 *
 * and stash interesting fields in module globals (declared in
 * acpi.h, consumed by stages D+).  No AML; no dynamic memory; the
 * parser runs entirely on the EFI-supplied table memory, which
 * UEFI marks ACPI_RECLAIM in the memory map -- meaning kappara
 * must NOT hand those pages out to the PMM.  Today the pmm
 * starts at __kernel_end and goes up, so it never overlaps the
 * lower-RAM ACPI region; when stage D's pmm starts consuming the
 * EFI memory map we'll honour ACPI_RECLAIM explicitly.
 *
 * Checksum policy: refuse to use a table whose 8-bit sum of its
 * declared `length` bytes isn't zero.  A failed checksum implies
 * the firmware handed us a corrupt or relocated-but-not-fixed-up
 * table; better to log + skip than wire stage D off bogus data.
 *
 * Boot path:
 *   - UEFI: efi_acpi_rsdp is set by efi_main; acpi_init walks it.
 *   - QEMU `-kernel`: efi_acpi_rsdp stays NULL.  acpi_init logs a
 *     one-liner and returns; the rest of kmain continues with the
 *     existing hardcoded virt constants (PLAT_GIC_*, etc).  This
 *     is fine for development; AWS Graviton boots through UEFI.
 */

#include "kappara/arch/acpi.h"
#include "kappara/core/printk.h"

/* ---- Globals (see acpi.h for what's exported) ---- */
int       acpi_present;
uint64_t  acpi_gicd_base;
uint8_t   acpi_gic_version;
uint64_t  acpi_gicr_base;
uint64_t  acpi_gicr_length;
uint32_t  acpi_nr_cpus;
uint64_t  acpi_cpu_mpidr[ACPI_MAX_CPUS];
uint64_t  acpi_pcie_ecam_base;
uint8_t   acpi_pcie_bus_start;
uint8_t   acpi_pcie_bus_end;
uint32_t  acpi_timer_ns_el1_gsiv;
uint32_t  acpi_timer_virt_el1_gsiv;
uint64_t  acpi_fadt_flags;

/* Filled in by efi_main; NULL under the `-kernel` boot path. */
extern void *efi_acpi_rsdp;

/* ---- Helpers ---- */

static uint8_t checksum8(const void *p, uint32_t n)
{
	const uint8_t *b = p;
	uint8_t s = 0;
	while (n--) s = (uint8_t)(s + *b++);
	return s;
}

static int sig_eq(const char *a, const char *b, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		if (a[i] != b[i]) return 0;
	return 1;
}

static const struct acpi_sdt_header *
xsdt_lookup(const struct acpi_xsdt *xsdt, const char *sig4)
{
	uint32_t total   = xsdt->header.length;
	uint32_t hdr_len = sizeof(struct acpi_sdt_header);
	if (total <= hdr_len) return 0;

	uint32_t nentries = (total - hdr_len) / sizeof(uint64_t);
	for (uint32_t i = 0; i < nentries; i++) {
		const struct acpi_sdt_header *h =
			(const struct acpi_sdt_header *)(uintptr_t)xsdt->entries[i];
		if (!h) continue;
		if (sig_eq(h->signature, sig4, 4)) return h;
	}
	return 0;
}

/* ---- MADT walker ---- */

static void parse_madt(const struct acpi_madt *madt)
{
	uint32_t total      = madt->header.length;
	uint32_t entries_off = (uint32_t)((const uint8_t *)madt->entries
					- (const uint8_t *)madt);
	if (total <= entries_off) return;

	const uint8_t *p   = madt->entries;
	const uint8_t *end = (const uint8_t *)madt + total;

	while (p + sizeof(struct acpi_madt_subhdr) <= end) {
		const struct acpi_madt_subhdr *sub =
			(const struct acpi_madt_subhdr *)p;
		if (sub->length < sizeof(*sub)) break;
		if (p + sub->length > end) break;

		switch (sub->type) {
		case ACPI_MADT_TYPE_GICC: {
			const struct acpi_madt_gicc *gicc =
				(const struct acpi_madt_gicc *)p;
			/* Bit 0 of flags = "Enabled" -- a disabled CPU is
			 * physically present but firmware tells us not to
			 * use it.  Park-protocol secondaries still have
			 * this set; that's fine. */
			if ((gicc->flags & 1) &&
			    acpi_nr_cpus < ACPI_MAX_CPUS)
				acpi_cpu_mpidr[acpi_nr_cpus++] = gicc->arm_mpidr;
			break;
		}
		case ACPI_MADT_TYPE_GICD: {
			const struct acpi_madt_gicd *gicd =
				(const struct acpi_madt_gicd *)p;
			acpi_gicd_base    = gicd->physical_base_address;
			acpi_gic_version  = gicd->gic_version;
			break;
		}
		case ACPI_MADT_TYPE_GICR: {
			const struct acpi_madt_gicr *gicr =
				(const struct acpi_madt_gicr *)p;
			acpi_gicr_base    = gicr->discovery_range_base_address;
			acpi_gicr_length  = gicr->discovery_range_length;
			break;
		}
		default:
			/* Ignore GIC_MSI, GIC_ITS, and any unknown types.
			 * They're useful for MSI-X (stage E+); for now we
			 * just skip them. */
			break;
		}

		p += sub->length;
	}
}

/* ---- MCFG walker (just take the first allocation) ---- */

static void parse_mcfg(const struct acpi_mcfg *mcfg)
{
	uint32_t total      = mcfg->header.length;
	uint32_t entries_off = (uint32_t)sizeof(*mcfg);
	if (total <= entries_off) return;

	const struct acpi_mcfg_alloc *alloc = &mcfg->allocations[0];
	acpi_pcie_ecam_base  = alloc->base_address;
	acpi_pcie_bus_start  = alloc->start_bus_number;
	acpi_pcie_bus_end    = alloc->end_bus_number;
}

/* ---- GTDT (just the EL1 timer GSIVs) ---- */

static void parse_gtdt(const struct acpi_gtdt *gtdt)
{
	acpi_timer_ns_el1_gsiv   = gtdt->non_secure_el1_timer_gsiv;
	acpi_timer_virt_el1_gsiv = gtdt->virtual_el1_timer_gsiv;
}

/* ---- FADT (just the flags word for now) ---- */

static void parse_fadt(const struct acpi_fadt_min *fadt)
{
	/* If the firmware advertises a FADT shorter than `flags` the
	 * field offset is unsafe; ACPI 6.0+ tables are always >=116
	 * bytes so this is essentially defensive. */
	if (fadt->header.length < sizeof(*fadt)) return;
	acpi_fadt_flags = fadt->flags;
}

/* ---- Entry point ---- */

void acpi_init(void)
{
	if (!efi_acpi_rsdp) {
		kprintf("acpi: no RSDP (booted via -kernel, not UEFI); "
			"using platform defaults\n");
		return;
	}

	const struct acpi_rsdp *rsdp = efi_acpi_rsdp;

	if (!sig_eq(rsdp->signature, "RSD PTR ", 8)) {
		kprintf("acpi: bad RSDP signature; skipping\n");
		return;
	}
	if (rsdp->revision < 2) {
		kprintf("acpi: ACPI 1.0 RSDP (rev %u); need 2.0+, skipping\n",
			rsdp->revision);
		return;
	}
	/* RSDP has two checksums.  The first 20 bytes match the 1.0
	 * layout (for backwards compat); the full `length` bytes are
	 * what 2.0+ tools care about.  Validate both. */
	if (checksum8(rsdp, 20) != 0 ||
	    checksum8(rsdp, rsdp->length) != 0) {
		kprintf("acpi: RSDP checksum bad; skipping\n");
		return;
	}

	const struct acpi_xsdt *xsdt =
		(const struct acpi_xsdt *)(uintptr_t)rsdp->xsdt_address;
	if (!xsdt || !sig_eq(xsdt->header.signature, "XSDT", 4) ||
	    checksum8(xsdt, xsdt->header.length) != 0) {
		kprintf("acpi: XSDT missing or bad checksum; skipping\n");
		return;
	}

	const struct acpi_sdt_header *h;

	/* MADT signature is "APIC" (legacy x86 carryover). */
	h = xsdt_lookup(xsdt, "APIC");
	if (h && checksum8(h, h->length) == 0)
		parse_madt((const struct acpi_madt *)h);

	h = xsdt_lookup(xsdt, "MCFG");
	if (h && checksum8(h, h->length) == 0)
		parse_mcfg((const struct acpi_mcfg *)h);

	h = xsdt_lookup(xsdt, "GTDT");
	if (h && checksum8(h, h->length) == 0)
		parse_gtdt((const struct acpi_gtdt *)h);

	/* FADT signature is "FACP" (legacy carryover). */
	h = xsdt_lookup(xsdt, "FACP");
	if (h && checksum8(h, h->length) == 0)
		parse_fadt((const struct acpi_fadt_min *)h);

	acpi_present = 1;

	kprintf("acpi: RSDP %p XSDT %p\n", rsdp, xsdt);
	kprintf("acpi: GIC v%u dist 0x%lx redist 0x%lx (%lu B)\n",
		acpi_gic_version, acpi_gicd_base,
		acpi_gicr_base, acpi_gicr_length);
	kprintf("acpi: %u CPUs", acpi_nr_cpus);
	for (uint32_t i = 0; i < acpi_nr_cpus; i++)
		kprintf("%s 0x%lx",
			i == 0 ? " mpidr" : ",",
			acpi_cpu_mpidr[i]);
	kprintf("\n");
	kprintf("acpi: PCIe ECAM 0x%lx bus %u..%u\n",
		acpi_pcie_ecam_base,
		acpi_pcie_bus_start, acpi_pcie_bus_end);
	kprintf("acpi: timer ns-el1 GSIV %u, virt-el1 GSIV %u\n",
		acpi_timer_ns_el1_gsiv,
		acpi_timer_virt_el1_gsiv);
}

/*
 * uts/virt/pcie.c -- AWS.md stage D: enumerate the PCIe bus via ECAM.
 *
 * Stage C left us an ECAM base + bus range in acpi_pcie_ecam_base /
 * acpi_pcie_bus_start / acpi_pcie_bus_end (zero when there's no
 * MCFG, i.e. the `-kernel` dev boot).  Stage D walks the resulting
 * config-space window and records every present function.
 *
 * The ECAM math is fixed by PCIe: each function's 4 KB config page
 * sits at  ecam_base + (bus<<20) + (dev<<15) + (fn<<12).  Vendor
 * ID 0xFFFF means "not present" (the bus pulls all-ones when there
 * is no responder).  Header type bit 7 says "multi-function" -- if
 * clear we only probe fn 0 of that dev to save a few bus cycles.
 *
 * The ECAM window is in our identity-map's Device region (PERIPH
 * range 0x08000000..0x40000000 on virt) so no per-call mapping is
 * needed.  Stage F's NVMe driver and stage E's ENA driver will
 * also live within this range on QEMU virt; AWS Graviton ECAM
 * sits at a similar low-ish address, similarly captured by our
 * Device-mapped identity map (Stage A's ImageBase=0x40080000
 * keeps the kernel RAM strictly above the Device window).
 *
 * One PCIe quirk we DO handle: a 64-bit BAR is encoded as TWO
 * consecutive 32-bit entries.  We still record both in bar[i] /
 * bar[i+1] but skip past i+1 when walking the rest.
 */

#include "kappara/arch/pcie.h"
#include "kappara/arch/acpi.h"
#include "kappara/arch/mmu.h"
#include "kappara/core/printk.h"

struct pci_device pci_devs[PCI_MAX_DEVS];
uint32_t          pci_nr_devs;

uint64_t pcie_ecam_base;
uint8_t  pcie_bus_start;
uint8_t  pcie_bus_end;

/* QEMU virt default ECAM: low-mem 16 MB region at 0x3F000000,
 * covers buses 0..15.  Already Device-mapped by build_identity_map
 * (sits in the PERIPH range 0x08000000..0x40000000).  We DON'T
 * fall back to this address under `-kernel` even though it's
 * always reserved on virt: when no PCIe device is wired in, gpex
 * doesn't back the underlying bytes and the first ECAM read
 * generates a synchronous external abort (ESR DFSC=0x10) instead
 * of returning the canonical 0xFFFF "no device" pattern.  Until we
 * either probe via the device tree or wire up a per-read fixup
 * handler, we skip enumeration unless ACPI gave us an MCFG. */

/* ---- MMIO config-space accessors ---- */

static volatile uint8_t *cfg_ptr(uint8_t bus, uint8_t dev, uint8_t fn,
                                 uint16_t off)
{
	uint64_t addr = pcie_ecam_base
	              + ((uint64_t)bus << 20)
	              + ((uint64_t)dev << 15)
	              + ((uint64_t)fn  << 12)
	              + off;
	return (volatile uint8_t *)(uintptr_t)addr;
}

static uint16_t cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
	return *(volatile uint16_t *)cfg_ptr(bus, dev, fn, off);
}

static uint32_t cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
	return *(volatile uint32_t *)cfg_ptr(bus, dev, fn, off);
}

static uint8_t cfg_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
	return *(volatile uint8_t *)cfg_ptr(bus, dev, fn, off);
}

/* ---- Capability list walker (MSI-X = 0x11) ---- */

static uint8_t find_msix_cap(uint8_t bus, uint8_t dev, uint8_t fn)
{
	uint16_t status = cfg_read16(bus, dev, fn, 0x06);
	if (!(status & 0x10)) return 0;	/* Capabilities List bit */

	uint8_t off = cfg_read8(bus, dev, fn, 0x34) & 0xFC;
	/* Bound the walk -- malformed devices can produce a loop. */
	for (int i = 0; off && i < 48; i++) {
		uint8_t id   = cfg_read8(bus, dev, fn, off);
		uint8_t next = cfg_read8(bus, dev, fn, off + 1) & 0xFC;
		if (id == 0x11) return off;
		off = next;
	}
	return 0;
}

/* ---- Per-function recorder ---- */

static const char *recognise(uint16_t vid, uint16_t did)
{
	if (vid == PCI_VID_AMAZON && did == PCI_DID_AMAZON_ENA)  return "ENA";
	if (vid == PCI_VID_AMAZON && did == PCI_DID_AMAZON_NVME) return "NVMe";
	return 0;
}

static void probe_function(uint8_t bus, uint8_t dev, uint8_t fn)
{
	uint16_t vid = cfg_read16(bus, dev, fn, 0x00);
	if (vid == 0xFFFF) return;

	if (pci_nr_devs >= PCI_MAX_DEVS) return;
	struct pci_device *p = &pci_devs[pci_nr_devs++];

	p->bus            = bus;
	p->dev            = dev;
	p->fn             = fn;
	p->vendor_id      = vid;
	p->device_id      = cfg_read16(bus, dev, fn, 0x02);
	p->revision       = cfg_read8 (bus, dev, fn, 0x08);
	p->prog_if        = cfg_read8 (bus, dev, fn, 0x09);
	p->class_subclass = cfg_read16(bus, dev, fn, 0x0A);
	p->header_type    = cfg_read8 (bus, dev, fn, 0x0E);

	/* BARs only meaningful for header type 0 (endpoint).  Headers
	 * type 1 (bridge) and 2 (cardbus) reuse bytes 0x10+ for other
	 * purposes; reading them is harmless but the values are not
	 * BARs. */
	if ((p->header_type & 0x7F) == 0) {
		for (int i = 0; i < 6; i++)
			p->bar[i] = cfg_read32(bus, dev, fn, 0x10 + 4 * i);
	}

	p->msix_cap = find_msix_cap(bus, dev, fn);

	const char *known = recognise(p->vendor_id, p->device_id);
	kprintf("pcie: %02x:%02x.%u %04x:%04x class %04x%s%s%s\n",
		bus, dev, fn,
		p->vendor_id, p->device_id, p->class_subclass,
		known ? " [" : "", known ? known : "", known ? "]" : "");
}

/* ---- Top-level enumeration ---- */

void pcie_init(void)
{
	if (!acpi_pcie_ecam_base) {
		kprintf("pcie: no ACPI MCFG (booted via -kernel); "
			"enumeration skipped\n");
		return;
	}

	pcie_ecam_base  = acpi_pcie_ecam_base;
	pcie_bus_start  = acpi_pcie_bus_start;
	pcie_bus_end    = acpi_pcie_bus_end;

	/* Make sure the ECAM window is reachable.  QEMU virt (with
	 * highmem PCIe) puts ECAM at 0x4010000000 = 256 GB + 256 MB,
	 * well outside the 0..2 GB chunk the boot identity map covers.
	 * Map the 1 GB block that contains the start; the full range
	 * fits in one block for any plausible bus_end because each bus
	 * is 1 MB of config space.  (For >256 buses, we'd need a
	 * multi-block loop; nothing real has that.)
	 * Guard: skip for addresses in the first 2 GB -- those are
	 * already wired by build_identity_map:
	 *   L1[0] → l2_table  (0..1 GB, with Device blocks for PERIPH)
	 *   L1[1] → l2_table_hi (1..2 GB, Normal RAM)
	 * Calling mmu_map_device_1gb for an address < 0x80000000 rounds
	 * its base to 0 and overwrites l1_table[0] with a 1 GB Device
	 * block, destroying the TABLE pointer to l2_table and making the
	 * entire 0..1 GB range inaccessible from EL0. */
	if (pcie_ecam_base >= 0x80000000UL)
		mmu_map_device_1gb(pcie_ecam_base);

	kprintf("pcie: ECAM 0x%lx bus %u..%u (ACPI MCFG)\n",
		pcie_ecam_base, pcie_bus_start, pcie_bus_end);

	for (uint32_t b = pcie_bus_start; b <= pcie_bus_end; b++) {
		for (uint32_t d = 0; d < 32; d++) {
			/* Probe fn 0 first; if vid==0xFFFF skip the slot.
			 * If header_type & 0x80 then this is a multi-fn
			 * device and we probe fn 1..7 too. */
			uint16_t v0 = cfg_read16(b, d, 0, 0x00);
			if (v0 == 0xFFFF) continue;

			probe_function(b, d, 0);
			uint8_t h = cfg_read8(b, d, 0, 0x0E);
			if (!(h & 0x80)) continue;

			for (uint32_t f = 1; f < 8; f++)
				probe_function(b, d, f);
		}
	}

	kprintf("pcie: enumeration complete -- %u device%s\n",
		pci_nr_devs, pci_nr_devs == 1 ? "" : "s");
}

/* ---- BAR decoders -------------------------------------------------- */
/* 32-bit memory BAR: type bits [2:1] = 00, addr = bar[i] & ~0xf.
 * 64-bit memory BAR: type bits [2:1] = 10, occupies bar[i] + bar[i+1].
 * I/O BAR (bit 0 = 1) we don't care about -- AArch64 has no PIO space
 * and modern PCIe assigns memory-mapped BARs to everything we need. */

int pcie_bar_is_64(const struct pci_device *p, unsigned idx)
{
	if (idx >= 6) return 0;
	uint32_t b = p->bar[idx];
	if (b & 0x1) return 0;			/* I/O BAR */
	return ((b >> 1) & 0x3) == 0x2;
}

uint64_t pcie_bar_addr(const struct pci_device *p, unsigned idx)
{
	if (idx >= 6) return 0;
	uint32_t b = p->bar[idx];
	if (b & 0x1) return 0;			/* I/O BAR */

	uint64_t lo = (uint64_t)(b & ~0xfu);
	if (((b >> 1) & 0x3) == 0x2) {
		if (idx + 1 >= 6) return 0;	/* malformed */
		uint64_t hi = (uint64_t)p->bar[idx + 1];
		return (hi << 32) | lo;
	}
	return lo;
}

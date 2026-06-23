/*
 * include/kappara/arch/pcie.h -- AWS.md stage D: PCIe ECAM enumeration.
 *
 * Walks the PCIe configuration space via ECAM (PCIe-specific
 * memory-mapped config window).  ECAM math:
 *
 *   cfg(seg=0, bus, dev, fn) = ecam_base + (bus<<20) + (dev<<15) + (fn<<12)
 *
 * Each function gets a 4 KB config page.  Stage D records every
 * present device (vendor != 0xFFFF) into pci_devs[]; stage E (ENA)
 * and stage F (NVMe) iterate this list to find their hardware.
 */
#ifndef KAPPARA_ARCH_PCIE_H
#define KAPPARA_ARCH_PCIE_H

#include <stdint.h>

/* AWS device IDs we care about.  Vendor 0x1d0f is Amazon. */
#define PCI_VID_AMAZON		0x1d0f
#define PCI_DID_AMAZON_ENA	0xec20
#define PCI_DID_AMAZON_NVME	0x8061

/* Per-device record.  bar[i] is the raw 32-bit BAR value; 64-bit
 * BARs occupy two consecutive entries.  We don't size BARs here --
 * that's the driver's job (write 0xFFFFFFFF, read back, mask, ~+1)
 * because sizing requires disabling the device while it's poked.
 *
 * msix_cap is the capability-list offset of the MSI-X cap (0 if
 * the device doesn't expose one).  Stage E will read MSI-X Message
 * Control + table offset from that cap. */
struct pci_device {
	uint8_t   bus, dev, fn;
	uint16_t  vendor_id;
	uint16_t  device_id;
	uint16_t  class_subclass;	/* (class << 8) | subclass */
	uint8_t   prog_if;
	uint8_t   revision;
	uint8_t   header_type;		/* 0 = endpoint, 1 = bridge */
	uint8_t   msix_cap;		/* 0 = not present */
	uint32_t  bar[6];
};

#define PCI_MAX_DEVS 32
extern struct pci_device pci_devs[PCI_MAX_DEVS];
extern uint32_t          pci_nr_devs;

/* Initialised by pcie_init: ECAM base actually in use (ACPI value
 * if present, else fallback). */
extern uint64_t pcie_ecam_base;
extern uint8_t  pcie_bus_start;
extern uint8_t  pcie_bus_end;

/* Enumerate the bus + populate pci_devs[].  Safe to call with no
 * MCFG -- falls back to QEMU virt's default ECAM (0x3F000000, 16
 * MB, buses 0..15) so the `-kernel` dev path still exercises the
 * code.  Logs one line per found device, plus a summary. */
void pcie_init(void);

#endif /* KAPPARA_ARCH_PCIE_H */

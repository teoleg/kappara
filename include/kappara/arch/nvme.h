/*
 * include/kappara/arch/nvme.h -- AWS.md stage F: NVMe block driver.
 *
 * Probes pci_devs[] for a PCI Class 0x0108 (Mass Storage / NVM
 * controller), brings the controller up via the standard NVMe
 * 1.4 init sequence, registers a struct block_device named
 * "nvme0n1" backing namespace 1.  Polled completion, single I/O
 * queue pair, one LBA per command (NLB = 0).
 *
 * On QEMU virt: tested via `-device nvme,drive=hd,serial=foo`.
 * On AWS Graviton: the NVMe BAR sits in the high-mem PCIe MMIO
 * window; mmu_map_device_1gb takes care of mapping it.
 */
#ifndef KAPPARA_ARCH_NVME_H
#define KAPPARA_ARCH_NVME_H

#include <stdint.h>

struct block_device;

/* Find an NVMe controller in pci_devs[] and bring it up.  Safe
 * (no-op) when no NVMe device is present, in which case nvme_get
 * returns NULL. */
void                  nvme_init(void);

/* Returns the registered block_device for namespace 1 of the first
 * NVMe controller, or NULL if there is none. */
struct block_device  *nvme_get(void);

/* Read-only diagnostic snapshot used by /proc/nvme.  All fields are
 * zeroed when the driver isn't initialised (no controller present
 * or bring-up failed). */
struct nvme_info {
	int       present;
	uint16_t  vid;
	char      model[41];
	char      serial[21];
	char      firmware[9];
	unsigned  major, minor, tertiary;
	uint64_t  bar0;
	uint64_t  ns1_blocks;
	uint32_t  ns1_lba_bytes;
};
void                  nvme_get_info(struct nvme_info *out);

#endif /* KAPPARA_ARCH_NVME_H */

/*
 * include/kappara/arch/ena.h -- AWS.md stage E: ENA network driver
 *
 * Amazon Elastic Network Adapter -- the NIC the Nitro platform
 * exposes to EC2 guests.  PCI vendor 0x1d0f (Amazon), device IDs
 * 0xec20 (most VFs) and 0xec21 (some older variants); class
 * 0x0200 (Network controller).
 *
 * This driver is the third "real device" we ship after pcie.c
 * and nvme.c, and it sits on the same plumbing: probe via
 * pci_devs[] for class+vendor match, map BAR0 with
 * mmu_map_device_1gb, allocate queue pages from the PMM, run
 * admin commands polled.
 *
 * !!!! BLIND IMPLEMENTATION WARNING !!!!
 *
 * Stage E was written without a QEMU NIC model to test against
 * (QEMU has no ENA emulation).  Every register offset, bitfield,
 * and admin-command structure here was reconstructed from
 * training-data memory of the upstream Linux ena driver
 * (drivers/net/ethernet/amazon/ena/) and the public ENA datasheet.
 * Before this code boots on real Graviton hardware, every
 * `XXX-verify-against-ena_regs_defs.h` and
 * `XXX-verify-against-ena_admin_defs.h` site in uts/virt/ena.c
 * MUST be cross-checked against amzn-drivers' canonical headers:
 *
 *   https://github.com/amzn/amzn-drivers/tree/master/kernel/linux/ena
 *
 * The structural skeleton (probe -> reset -> admin queue ->
 * GET_FEATURE -> create I/O queues -> netif wire-up) follows
 * the spec faithfully; the byte-level constants are educated
 * best-effort.  Expect at least a couple of off-by-one offsets
 * to show up on first boot, fixable from kernel-log inspection.
 */
#ifndef KAPPARA_ARCH_ENA_H
#define KAPPARA_ARCH_ENA_H

void ena_init(void);

#endif /* KAPPARA_ARCH_ENA_H */

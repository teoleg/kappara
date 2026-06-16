/*
 * arch/aarch64/platform/virt.h -- QEMU `virt` machine layout
 *
 * The virt machine is QEMU's generic aarch64 virtual board: GIC v2,
 * PL011 UART, virtio-mmio bus, PSCI for SMP, no firmware mailbox or
 * framebuffer.  It's our second supported board (after BCM2837/Pi 3)
 * and the one we use for outside-world networking: virtio-net drops
 * into ip_lowers[] as eth0, then `-netdev user,hostfwd=tcp::N-:23`
 * exposes kappara's telnet port to whichever machine is running QEMU.
 *
 * Memory map (`qemu-system-aarch64 -M virt,gic-version=2`)
 * --------------------------------------------------------
 *   0x00000000..0x07ffffff   flash / boot ROM
 *   0x08000000..0x08000fff   GIC distributor
 *   0x08010000..0x08010fff   GIC CPU interface
 *   0x09000000..0x09000fff   PL011 UART
 *   0x0a000000..0x0a003fff   virtio-mmio (32 slots * 0x200, but lay
 *                            in 0x200 strides; virtio devices appear
 *                            here in QEMU command-line order)
 *   0x40000000..             RAM (-m N gives N MB; default 128 MB)
 *
 * No mailbox/framebuffer/mini-UART/BCM2836 IPI block here -- the
 * Pi-specific drivers compile out on `ARCH=virt` via the Makefile's
 * object list.  Cross-CPU IPI is GIC SGI 0 instead of the BCM2836
 * mailbox 0; the timer routes through the GIC's per-CPU PPI 30 (the
 * non-secure physical timer's standard PPI) instead of the BCM2836
 * per-core timer-control register.
 */

#ifndef KAPPARA_PLATFORM_VIRT_H
#define KAPPARA_PLATFORM_VIRT_H

#define PLAT_NAME			"qemu virt (generic aarch64)"

/* Physical memory layout. */
#define PLAT_PERIPH_BASE		0x08000000UL
#define PLAT_PERIPH_END			0x40000000UL
#define PLAT_RAM_BASE			0x40000000UL
#define PLAT_RAM_END			(PLAT_RAM_BASE + 0x10000000UL) /* 256 MB */

/* PL011 UART. */
#define PLAT_PL011_BASE			0x09000000UL

/* GIC v3.  Cortex-A53/A72 are GIC v3-capable and reset with the
 * system-register interface live; QEMU virt with the default
 * gic-version=3 puts the distributor at the v2 address and the
 * redistributors at 0x080A0000+0x20000*cpu.  The CPU interface
 * itself isn't MMIO -- it's ICC_* system registers. */
#define PLAT_GIC_DIST_BASE		0x08000000UL
#define PLAT_GIC_REDIST_BASE		0x080A0000UL
#define PLAT_GIC_REDIST_STRIDE		0x20000UL

/* Generic-timer IRQ.  The non-secure physical timer is PPI 30 (= INTID
 * 30) on every CPU; the GIC distributor exposes its enable + priority
 * via the per-CPU GICD_ISENABLER0 region. */
#define PLAT_TIMER_PPI			30

/* IPI = SGI 0 (software-generated interrupt, broadcast or targeted via
 * GICD_SGIR). */
#define PLAT_IPI_SGI			0

/* virtio-mmio bus base.  Devices appear at +0x200 strides up to
 * MAX_VIRTIO_DEVICES slots.  Device order matches the QEMU command-line
 * order of `-device virtio-net-device,...` etc. */
#define PLAT_VIRTIO_MMIO_BASE		0x0A000000UL
#define PLAT_VIRTIO_MMIO_STRIDE		0x200UL
#define PLAT_VIRTIO_MMIO_MAX		32

#endif

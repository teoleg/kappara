/*
 * include/kappara/arch/virtio_net.h -- virtio-net driver (virt only)
 *
 * Only built on ARCH=virt; raspi3b doesn't have a virtio bus.  Phase
 * 2 of the road to telnet: probe + ring bring-up + raw TX/IRQ hooks.
 * Phase 3 hangs the netif under IP and wires up rx -> mblk -> IP demux.
 */

#ifndef KAPPARA_ARCH_VIRTIO_NET_H
#define KAPPARA_ARCH_VIRTIO_NET_H

void virtio_net_init(void);
void virtio_net_irq (void);

/* Phase 2 raw TX entry: byte-buffer send.  Phase 3 replaces this with
 * the netif streamtab + mblk path. */
int  virtio_net_tx_bytes(const void *buf, unsigned len);

#endif

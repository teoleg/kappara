/*
 * uts/virt/miniuart.c -- mini-UART stubs for QEMU virt
 *
 * The virt machine has no second AUX UART.  These exist only so
 * uts/os/net/slip.c links cleanly when compiled for virt -- slip_init
 * isn't called from the virt boot path (main.c will conditionalise
 * via #ifdef PLATFORM_VIRT), but the slip-streamtab references the
 * symbols at definition time, hence the stubs.
 *
 * Phase 3 (virtio-net) makes slip moot on virt -- the eth0 netif
 * carries all of the outside-world traffic.
 */

#include "kappara/arch/miniuart.h"

void miniuart_init(unsigned baud)         { (void)baud; }
void miniuart_tx_byte(unsigned char c)    { (void)c; }
int  miniuart_rx_nonblock(void)           { return -1; }

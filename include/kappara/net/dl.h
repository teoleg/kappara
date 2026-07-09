/*
 * include/kappara/net/dl.h -- raw datalink provider registration
 *
 * Driver-facing side of the mini-DLPI raw access path (net/dlpi.h
 * is the userland ABI; docs/DLPI.md the design).  Ethernet drivers
 * register once at init and tap every received frame into
 * dl_input(); userland reaches the device via /dev/<name>.
 */

#ifndef KAPPARA_NET_DL_H
#define KAPPARA_NET_DL_H

#include <stdint.h>

struct dlif;

struct dlif *dl_register(const char *name, const uint8_t *mac, unsigned mtu,
			 int (*tx)(void *cookie, const void *frame,
			           unsigned len),
			 void *cookie);

/* Driver RX tap: called with every complete received Ethernet
 * frame; fans copies out to matching open raw streams.  Cheap
 * no-op when nothing is open. */
void dl_input(struct dlif *d, const void *frame, unsigned len);

#endif

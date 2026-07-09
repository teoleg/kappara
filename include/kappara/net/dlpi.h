/*
 * include/kappara/net/dlpi.h -- mini-DLPI datalink access
 *
 * Shared kernel/userland ABI for raw datalink streams (/dev/eth0).
 * A deliberate subset of SVR4 DLPI (real primitive numbers, short
 * structs): DL_INFO for identity, DL_BIND to filter by SAP
 * (= ethertype; 0 receives everything), then raw M_DATA carrying
 * complete Ethernet frames in both directions -- the DLIOCRAW
 * convention instead of DL_UNITDATA envelopes.  No attach/detach
 * (one PPA per device), no promiscuity levels, no multicast.
 * See docs/DLPI.md.
 *
 * Userland flow (dhcpagent):
 *   fd = open("/dev/eth0")
 *   putmsg(DL_INFO_REQ)  -> getmsg(DL_INFO_ACK)   mac + mtu
 *   putmsg(DL_BIND_REQ{sap}) -> getmsg(DL_BIND_ACK)
 *   write(full frame) / read(full frame)
 */

#ifndef KAPPARA_NET_DLPI_H
#define KAPPARA_NET_DLPI_H

#include <stdint.h>

/* SVR4 DLPI primitive numbers (subset). */
#define DL_INFO_REQ	0x00
#define DL_BIND_REQ	0x01
#define DL_INFO_ACK	0x03
#define DL_BIND_ACK	0x04
#define DL_ERROR_ACK	0x05

struct dl_info_req {
	uint32_t dl_primitive;		/* DL_INFO_REQ */
};

struct dl_info_ack {
	uint32_t dl_primitive;		/* DL_INFO_ACK */
	uint32_t dl_mtu;
	uint32_t dl_sap;		/* currently bound sap (0 = all) */
	uint8_t  dl_mac[6];
	uint8_t  _pad[2];
};

struct dl_bind_req {
	uint32_t dl_primitive;		/* DL_BIND_REQ */
	uint32_t dl_sap;		/* ethertype filter; 0 = all */
};

struct dl_bind_ack {
	uint32_t dl_primitive;		/* DL_BIND_ACK */
	uint32_t dl_sap;
};

struct dl_error_ack {
	uint32_t dl_primitive;		/* DL_ERROR_ACK */
	uint32_t dl_error_primitive;	/* the request that failed */
};

#endif

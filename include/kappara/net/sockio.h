/*
 * include/kappara/net/sockio.h -- interface plumbing ioctls
 *
 * Shared kernel/userland ABI (cmd binaries include this directly,
 * same as icmp.h).  Solaris shape: the ioctl travels as M_IOCTL
 * down any stream with ip at the bottom -- /dev/udp is the
 * canonical choice -- and the IP multiplexor answers M_IOCACK.
 * dhcpagent and ifconfig are the intended callers.
 *
 * SIOCSIFGW is kappara-local: SVR4 would grow a route table and
 * SIOCADDRT; we have exactly one gateway per netif and the ioctl
 * is honest about that.  Revisit when a second Ethernet-class
 * interface exists (docs/DLPI.md).
 */

#ifndef KAPPARA_NET_SOCKIO_H
#define KAPPARA_NET_SOCKIO_H

#include <stdint.h>

struct kifreq {
	char     ifr_name[8];	/* "eth0", NUL-terminated */
	uint32_t ifr_addr;	/* host byte order */
	uint8_t  ifr_mac[6];	/* SIOCGIFHWADDR result */
	uint8_t  _pad[2];
};

#define SIOCSIFADDR	(('i' << 8) | 12)	/* set interface address */
#define SIOCGIFADDR	(('i' << 8) | 13)	/* get interface address */
#define SIOCSIFNETMASK	(('i' << 8) | 22)	/* set netmask */
#define SIOCGIFNETMASK	(('i' << 8) | 25)	/* get netmask */
#define SIOCSIFGW	(('i' << 8) | 40)	/* set default gateway */
#define SIOCGIFGW	(('i' << 8) | 41)	/* get default gateway */
#define SIOCGIFHWADDR	(('i' << 8) | 42)	/* get MAC */

#endif

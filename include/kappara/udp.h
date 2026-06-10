/*
 * include/kappara/udp.h -- UDP transport layer
 *
 * Minimal UDP: enough to send and receive datagrams.  No per-socket
 * binding table yet -- that comes with /dev/udp (TPI, N2).  For now
 * the direct-C API is what the kernel itself uses and what udp_input
 * exposes to ip_input.
 *
 * Checksum: RFC 768 UDP checksum is optional (zero means "not
 * computed").  We send with zero; incoming checksums are accepted
 * whether zero or valid.
 */

#ifndef KAPPARA_UDP_H
#define KAPPARA_UDP_H

#include <stdint.h>

struct netif;
struct msgb;
typedef struct msgb mblk_t;

#define UDP_HDR_LEN	8

struct udp_hdr {
	uint16_t src_port;	/* big-endian */
	uint16_t dst_port;	/* big-endian */
	uint16_t len;		/* big-endian: header + payload bytes */
	uint16_t checksum;	/* 0 = not computed */
} __attribute__((packed));

/* Called from ip_input after the IP header has been stripped.
 * src/dst are host byte order.  Caller hands ownership of mp. */
void udp_input(struct netif *nif, uint32_t src, uint32_t dst, mblk_t *mp);

/* Build and send a UDP datagram.  dst_ip/ports in host byte order.
 * mp's b_rptr..b_wptr is the UDP payload.  Returns 0 on success,
 * -1 on routing / alloc failure (mp freed on -1). */
int  udp_output(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                mblk_t *mp);

#endif

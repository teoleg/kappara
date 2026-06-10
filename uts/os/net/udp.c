/*
 * uts/os/net/udp.c -- UDP transport
 *
 * Phase N1: protocol layer only.  ip_input dispatches datagrams here;
 * we strip the header and log the arrival (no bound sockets yet, so
 * every incoming datagram is dropped after logging).  udp_output
 * prepends the 8-byte UDP header and calls ip_output.
 *
 * Phase N2 will add a per-stream binding table and the /dev/udp
 * STREAMS cdev with TPI-shaped M_PROTO messages (T_BIND_REQ,
 * T_UNITDATA_REQ, T_UNITDATA_IND) so user-space can open /dev/udp,
 * bind a port, and exchange datagrams -- exactly what SVR4's UDP
 * module does.
 *
 * Checksum policy
 * ---------------
 * Outgoing: zero (RFC 768 allows it; saves a full-packet scan in the
 * loopback path).  Incoming: accepted regardless of checksum value.
 */

#include <stdint.h>

#include "kappara/ip.h"
#include "kappara/netif.h"
#include "kappara/printk.h"
#include "kappara/streams.h"
#include "kappara/string.h"
#include "kappara/udp.h"

void udp_input(struct netif *nif, uint32_t src, uint32_t dst, mblk_t *mp)
{
	(void)nif; (void)dst;
	unsigned len = (unsigned)(mp->b_wptr - mp->b_rptr);
	if (len < UDP_HDR_LEN) {
		kprintf("udp_input: runt %u\n", len);
		freemsg(mp);
		return;
	}
	struct udp_hdr *h = (struct udp_hdr *)mp->b_rptr;
	uint16_t sport = ntohs16(h->src_port);
	uint16_t dport = ntohs16(h->dst_port);

	/* Strip the header so any future socket deliver sees only payload. */
	mp->b_rptr += UDP_HDR_LEN;

	kprintf("udp: %u.%u.%u.%u:%u -> :%u len=%u (no socket)\n",
		(src >> 24) & 0xff, (src >> 16) & 0xff,
		(src >>  8) & 0xff,  src        & 0xff,
		sport, dport,
		(unsigned)(mp->b_wptr - mp->b_rptr));
	freemsg(mp);
}

int udp_output(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
               mblk_t *mp)
{
	if (!mp) return -1;

	/* Need IP_HDR_LEN + UDP_HDR_LEN headroom.  mp must already carry
	 * IP_HDR_LEN of headroom (callers responsible, matching icmp). */
	if ((size_t)(mp->b_rptr - mp->b_datap->db_base) < UDP_HDR_LEN) {
		kprintf("udp_output: no UDP headroom\n");
		freemsg(mp);
		return -1;
	}
	mp->b_rptr -= UDP_HDR_LEN;

	unsigned payload_and_hdr = (unsigned)(mp->b_wptr - mp->b_rptr);
	struct udp_hdr *h = (struct udp_hdr *)mp->b_rptr;
	h->src_port = htons16(src_port);
	h->dst_port = htons16(dst_port);
	h->len      = htons16((uint16_t)payload_and_hdr);
	h->checksum = 0;	/* RFC 768: zero = not computed */

	return ip_output(dst_ip, IPPROTO_UDP, mp);
}

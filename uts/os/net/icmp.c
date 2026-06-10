/*
 * kernel/net/icmp.c -- ICMP echo handling
 *
 * The IP layer dispatches ICMP datagrams here.  We auto-reply to
 * type 8 (echo request) by flipping the type byte and recomputing
 * the checksum, then ip_output the same mblk back.  Type 0 (echo
 * reply) is stashed on a small in-kernel waiter queue so callers
 * blocked in icmp_wait_reply can observe their pong.
 *
 * Headroom discipline
 * -------------------
 * allocb gives us a buffer with b_rptr==b_wptr==db_base.  ip_output
 * needs IP_HDR_LEN bytes of headroom before b_rptr.  So when we
 * build a fresh ICMP mblk, we reserve IP_HDR_LEN by advancing both
 * pointers, then write the ICMP header + payload, then push to
 * ip_output which decrements b_rptr to expose the IP header slot.
 *
 * Reply auto-handling on the same mblk
 * ------------------------------------
 * The request mblk arrived from ip_input with b_rptr already past
 * the IP header (IP stripped).  So we have IP headroom intact -- we
 * can flip the ICMP type, recompute checksum, and re-send via
 * ip_output with the same mblk.  Net allocation: zero on the reply
 * path.  Real Solaris does the same trick.
 */

#include <stdint.h>

#include "kappara/icmp.h"
#include "kappara/ip.h"
#include "kappara/kmem.h"
#include "kappara/netif.h"
#include "kappara/printk.h"
#include "kappara/sched.h"
#include "kappara/streams.h"
#include "kappara/string.h"

/* ---- reply waiter ------------------------------------------------- */

/* Single-slot waiter queue.  Real Solaris keeps a per-stream list
 * (one stream per icmp socket); here we just need one slot for the
 * boot selftest and for cmd/ping (which serialises requests).  A
 * second concurrent ping would race; documented limitation. */
static struct {
	int       waiting;
	uint16_t  id;
	uint16_t  seq;
	int       got;
} icmp_wq;

static void icmp_deliver_reply(uint16_t id, uint16_t seq)
{
	if (!icmp_wq.waiting) return;
	if (icmp_wq.id != id || icmp_wq.seq != seq) return;
	icmp_wq.got = 1;
}

void icmp_arm_waiter(uint16_t id, uint16_t seq)
{
	icmp_wq.id      = id;
	icmp_wq.seq     = seq;
	icmp_wq.got     = 0;
	icmp_wq.waiting = 1;
}

int icmp_wait_reply(uint16_t id, uint16_t seq, unsigned timeout_ms)
{
	(void)id; (void)seq; (void)timeout_ms;
	/* lo0 loops synchronously, so the reply has already arrived
	 * by the time we get here.  Yield a couple of times for
	 * future async drivers. */
	for (int i = 0; i < 32 && !icmp_wq.got; i++)
		kthread_yield();

	int r = icmp_wq.got ? 0 : -1;
	icmp_wq.waiting = 0;
	return r;
}

/* ---- input -------------------------------------------------------- */

void icmp_input(struct netif *nif, uint32_t src, uint32_t dst, mblk_t *mp)
{
	(void)dst;
	unsigned len = (unsigned)(mp->b_wptr - mp->b_rptr);
	if (len < ICMP_HDR_LEN) {
		kprintf("icmp_input: runt %u\n", len);
		freemsg(mp);
		return;
	}
	struct icmp_hdr *h = (struct icmp_hdr *)mp->b_rptr;

	/* RFC 1071 checksum over the full ICMP body must equal zero
	 * if the packet is intact. */
	if (ip_checksum(h, len) != 0) {
		kprintf("icmp_input: checksum fail\n");
		freemsg(mp);
		return;
	}

	if (h->type == ICMP_TYPE_ECHO_REPLY) {
		icmp_deliver_reply(ntohs16(h->id), ntohs16(h->seq));
		freemsg(mp);
		return;
	}

	if (h->type == ICMP_TYPE_ECHO_REQUEST) {
		/* Flip type to reply, zero checksum, recompute, send. */
		h->type     = ICMP_TYPE_ECHO_REPLY;
		h->checksum = 0;
		uint16_t cs = ip_checksum(h, len);
		h->checksum = htons16(cs);
		(void)nif;
		/* Reply goes back to the original source. */
		if (ip_output(src, IPPROTO_ICMP, mp) < 0) {
			/* ip_output freed mp on failure */
		}
		return;
	}

	/* Other ICMP types (destination unreachable, etc.) -- drop for
	 * now.  Not relevant until we have actual routing failures. */
	freemsg(mp);
}

/* ---- outgoing echo ------------------------------------------------ */

int icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                   const void *payload, unsigned len)
{
	/* Reserve IP header room + ICMP header + payload. */
	unsigned sz = IP_HDR_LEN + ICMP_HDR_LEN + len;
	mblk_t *mp = allocb(sz, 0);
	if (!mp) return -1;

	/* Skip past the IP headroom -- ip_output will rewind b_rptr to
	 * fill it in. */
	mp->b_rptr += IP_HDR_LEN;
	mp->b_wptr  = mp->b_rptr;

	struct icmp_hdr *h = (struct icmp_hdr *)mp->b_wptr;
	h->type     = ICMP_TYPE_ECHO_REQUEST;
	h->code     = 0;
	h->checksum = 0;
	h->id       = htons16(id);
	h->seq      = htons16(seq);
	mp->b_wptr += ICMP_HDR_LEN;

	if (len > 0 && payload) {
		kmemcpy(mp->b_wptr, payload, len);
		mp->b_wptr += len;
	}

	uint16_t cs = ip_checksum(h, ICMP_HDR_LEN + len);
	h->checksum = htons16(cs);

	return ip_output(dst_ip, IPPROTO_ICMP, mp);
}

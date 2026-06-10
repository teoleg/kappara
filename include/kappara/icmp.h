/*
 * include/kappara/icmp.h -- ICMP message handling
 *
 * Just enough ICMP to make `ping` work both ways:
 *   - Incoming type 8 (echo request)  -> auto-reply with type 0.
 *   - Incoming type 0 (echo reply)    -> stash on a small in-kernel
 *                                         waiter queue so callers
 *                                         (cmd/ping or net_selftest)
 *                                         can observe their pong.
 *
 * The ICMP layer does not yet present a STREAMS device.  cmd/ping in
 * the next commit will open /dev/icmp; for this commit, only the
 * boot-time net_selftest exercises the round-trip from in-kernel
 * code.
 *
 * Echo request body (after the 8-byte ICMP header) is the optional
 * "payload" -- conventionally a timestamp or sequence pattern.  We
 * pass it through verbatim in the reply.
 */

#ifndef KAPPARA_ICMP_H
#define KAPPARA_ICMP_H

#include <stdint.h>

struct netif;
struct msgb;
typedef struct msgb mblk_t;

#define ICMP_TYPE_ECHO_REPLY	0
#define ICMP_TYPE_ECHO_REQUEST	8

struct icmp_hdr {
	uint8_t  type;
	uint8_t  code;
	uint16_t checksum;	/* big-endian on the wire */
	uint16_t id;		/* big-endian */
	uint16_t seq;		/* big-endian */
} __attribute__((packed));

#define ICMP_HDR_LEN	8

/* Called from ip_input after the IP header has been stripped.  mp's
 * b_rptr..b_wptr is the ICMP body (header + payload).  src/dst are
 * host byte order.  Caller hands ownership of mp. */
void icmp_input(struct netif *nif, uint32_t src, uint32_t dst, mblk_t *mp);

/* Build and send an ICMP echo request to dst_ip.  payload bytes are
 * copied into the body after the header.  id+seq are the conventional
 * fields the receiver echoes back.  Returns 0 on success, -1 on
 * routing / alloc failure. */
int  icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                    const void *payload, unsigned len);

/* Sleep until either an echo reply matching (id, seq) arrives or
 * `timeout_ms` elapses.  Returns 0 if a reply was seen, -1 on
 * timeout.  Pair with icmp_arm_waiter BEFORE sending so the
 * synchronous-loopback reply has somewhere to land. */
int  icmp_wait_reply(uint16_t id, uint16_t seq, unsigned timeout_ms);

/* Arm the single-slot reply waiter.  Must be called before the
 * matching icmp_send_echo so an immediately-delivered reply (lo0
 * loopback) finds the slot ready. */
void icmp_arm_waiter(uint16_t id, uint16_t seq);

/* ---- /dev/icmp STREAMS cdev ----------------------------------------
 *
 * User-kernel protocol: write a struct icmp_ping_req, read back a
 * struct icmp_ping_rep.  Both are M_DATA blocks.  Fields are in host
 * byte order.
 */

struct icmp_ping_req {
	uint32_t dst_ip;	/* host byte order */
	uint16_t id;
	uint16_t seq;
};

struct icmp_ping_rep {
	uint32_t src_ip;	/* host byte order */
	uint16_t id;
	uint16_t seq;
	int16_t  rtt_ms;	/* 0 = synchronous loopback, -1 = timeout */
};

/* Arm a STREAMS-based reply waiter.  rq is the driver's read-side
 * queue; the reply mblk (struct icmp_ping_rep) is delivered there. */
struct queue;
void icmp_arm_waiter_stream(uint16_t id, uint16_t seq, struct queue *rq,
                            uint32_t dst_ip);

/* Register the /dev/icmp STREAMS cdev. */
void icmp_str_init(void);

#endif

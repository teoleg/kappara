/*
 * include/kappara/net/icmp.h -- ICMP as a STREAMS module above IP
 *
 * After the N2c rewrite ICMP is a STREAMS module (not a driver):
 * /dev/icmp opens an IP-driver stream and stream_open auto-pushes
 * the "icmp" module on top.  The module:
 *
 *   - Auto-assigns a 16-bit ICMP id at qopen time and binds proto=1
 *     to IP via M_PROTO{IP_T_BIND_REQ}.  Multiple concurrent opens
 *     each get their own id and bind separately; IP demux multicasts
 *     proto=1 packets to all bound uppers via dupmsg, and each
 *     module instance filters the incoming flow by its assigned id.
 *     This is how Solaris does raw-ICMP endpoints.
 *
 *   - User write: struct icmp_ping_req{dst_ip, seq}.  The module
 *     stamps its own id into the outgoing packet; user doesn't
 *     specify id.  Module arms a waiter for (id, seq) on its read
 *     side before sending so the reply has somewhere to land.
 *
 *   - User read: struct icmp_ping_rep{src_ip, seq, rtt_ms}.
 *
 *   - Type-8 echo requests aimed at one of our netifs get auto-
 *     replied: build the reply mblk, putnext DOWN through the same
 *     stack to IP.  Because we filter by our own id first, the
 *     auto-reply path only fires when the requester used our id.
 *     For the boot selftest that loopbacks against itself, that's
 *     exactly the case.
 */

#ifndef KAPPARA_ICMP_H
#define KAPPARA_ICMP_H

#include <stdint.h>

struct streamtab;
struct queue;
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

/* ---- /dev/icmp STREAMS shape -- user/kernel ABI ------------------- */
/*
 * Write a struct icmp_ping_req, read back a struct icmp_ping_rep.
 * Both are M_DATA blocks.  Fields are in host byte order.
 *
 * Note: the kernel assigns and stamps the ICMP `id` field.  User
 * code never sees it.  This mirrors UDP's hidden ephemeral source
 * port and lets concurrent /dev/icmp opens coexist -- each gets
 * its own id, each filters incoming replies by that id.
 */
struct icmp_ping_req {
	uint32_t dst_ip;	/* host byte order */
	uint16_t seq;		/* user-chosen; echoed back */
	uint16_t _pad;
};

struct icmp_ping_rep {
	uint32_t src_ip;	/* host byte order */
	uint16_t seq;
	int16_t  rtt_ms;	/* 0 = synchronous loopback, -1 = timeout */
};

/* The icmp module's streamtab.  stream_head's autopush calls into
 * this when /dev/icmp gets opened. */
extern struct streamtab icmp_streamtab;

/* Boot self-test driver: opens an icmp stream in-kernel, fires off
 * an echo to `dst`, waits for the reply.  Returns 0 on success, -1
 * on failure.  Used by net_selftest. */
int icmp_selftest_run(uint32_t dst, uint16_t seq);

/* Module init (called from streams_head_init).  Registers
 * icmp_streamtab in the by-name module registry so I_PUSH "icmp"
 * (and the /dev/icmp autopush) can find it. */
void icmp_module_init(void);

#endif

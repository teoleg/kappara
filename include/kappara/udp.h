/*
 * include/kappara/udp.h -- UDP as a STREAMS module above IP, with
 *                          a TPI-shaped M_PROTO user ABI.
 *
 * /dev/udp's cdev maps to ip_streamtab.  stream_open autopushes
 * the "udp" module on top, exactly the same shape as ICMP.  Once
 * pushed, the user-visible stream is udp-over-ip.
 *
 * TPI (Transport Provider Interface) primitives are M_PROTO mblks
 * tagged at offset 0 with a `prim` byte.  putmsg sends them down,
 * getmsg pulls them up.  Standard SVR4.  In TLI/XTI shorthand:
 *
 *   T_BIND_REQ      bind to a local port            (user -> kernel)
 *   T_BIND_ACK      bind succeeded                  (kernel -> user)
 *   T_BIND_NAK      bind failed                     (kernel -> user)
 *   T_UNITDATA_REQ  send datagram + M_DATA payload  (user -> kernel)
 *   T_UNITDATA_IND  datagram arrived + M_DATA       (kernel -> user)
 *
 * Datagrams are carried in the M_DATA b_cont continuation of the
 * M_PROTO mblk; the M_PROTO header carries the connection info
 * (local/remote addrs/ports).
 *
 * Real SVR4 has many more primitives (T_INFO_REQ, T_CONN_REQ for
 * connection-oriented, T_OPTMGMT_REQ for socket options, ...).
 * We ship the minimal datagram-oriented set.
 */

#ifndef KAPPARA_UDP_H
#define KAPPARA_UDP_H

#include <stdint.h>

struct streamtab;
struct queue;
struct msgb;
typedef struct msgb mblk_t;

#define UDP_HDR_LEN	8

struct udp_hdr {
	uint16_t src_port;	/* big-endian */
	uint16_t dst_port;	/* big-endian */
	uint16_t len;		/* big-endian: header + payload bytes */
	uint16_t checksum;	/* 0 = not computed */
} __attribute__((packed));

/* ---- TPI primitives ----------------------------------------------- */

#define T_BIND_REQ	1
#define T_BIND_ACK	2
#define T_BIND_NAK	3
#define T_UNITDATA_REQ	4
#define T_UNITDATA_IND	5

struct t_bind_req {
	uint8_t  prim;		/* T_BIND_REQ */
	uint8_t  _pad[1];
	uint16_t port;		/* host byte order; 0 = bind ephemeral */
};

struct t_bind_ack {
	uint8_t  prim;		/* T_BIND_ACK */
	uint8_t  _pad[1];
	uint16_t port;		/* the actually-bound port */
};

struct t_bind_nak {
	uint8_t  prim;		/* T_BIND_NAK */
	uint8_t  reason;	/* 1 = address in use */
	uint8_t  _pad[2];
};

struct t_unitdata_req {
	uint8_t  prim;		/* T_UNITDATA_REQ */
	uint8_t  _pad[3];
	uint32_t dst_ip;	/* host byte order */
	uint16_t dst_port;
	uint16_t _pad2;
	/* payload chained in b_cont as M_DATA */
};

struct t_unitdata_ind {
	uint8_t  prim;		/* T_UNITDATA_IND */
	uint8_t  _pad[3];
	uint32_t src_ip;
	uint16_t src_port;
	uint16_t _pad2;
	/* payload chained in b_cont as M_DATA */
};

/* The udp module's streamtab.  stream_head's autopush calls in
 * when /dev/udp gets opened. */
extern struct streamtab udp_streamtab;

/* Module init: registers udp_streamtab in the by-name registry so
 * I_PUSH "udp" (and the /dev/udp autopush) can find it, and binds
 * /dev/udp's cdev entry to ip_streamtab. */
void udp_module_init(void);

/* Boot self-test: open a udp stream, T_BIND_REQ port=12345,
 * T_UNITDATA_REQ to 127.0.0.1:12345 with a known payload, getmsg
 * to receive the loopback T_UNITDATA_IND, verify the payload. */
int  udp_selftest_run(void);

#endif

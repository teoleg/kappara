/*
 * include/kappara/net/tcp.h -- TCP as a STREAMS module above IP, with
 *                              a TPI-shaped M_PROTO user ABI.
 *
 * Same architectural shape as ICMP and UDP: pushable STREAMS module,
 * not a driver.  /dev/tcp's cdev maps to ip_streamtab; stream_open
 * autopushes "tcp" on top.  Per-open state is one TCB (transmission
 * control block) behind q_ptr -- one stream = one connection
 * endpoint.
 *
 * User ABI is the SVR4 TPI primitive set delivered via putmsg /
 * getmsg.  Each primitive is an M_PROTO mblk with a leading prim
 * byte; per-primitive payload follows in the same mblk; user data
 * (for T_DATA_REQ / T_DATA_IND) chains as b_cont(M_DATA).
 *
 * Primitives ordered by typical client / server flow:
 *
 *                 client side                server side
 *   bind          T_BIND_REQ  -->                       T_BIND_REQ
 *                       <--  T_BIND_ACK                       T_BIND_ACK
 *                                            (listener loop)
 *   connect       T_CONN_REQ  -->            <--  (3-way handshake)
 *                       <--  T_CONN_CON                       T_CONN_IND
 *                                                             T_CONN_RES
 *                                                                  -->
 *   send / recv   T_DATA_REQ  + M_DATA  -->
 *                       <--  T_DATA_IND + M_DATA
 *   close one direction (graceful)
 *                 T_ORDREL_REQ -->
 *                       <--  T_ORDREL_IND
 *   abort         T_DISCON_REQ -->
 *                       <--  T_DISCON_IND
 *
 * TCP's IP bind uses the same ip_bind_meta key field UDP does, but
 * encodes BOTH the local port AND the remote endpoint (when known)
 * so IP's exact-match demux delivers a segment to the right
 * connection without TCP having to multicast and filter.  See
 * the `key` layout in uts/os/net/tcp.c -- it packs as
 * (local_port << 16) | (remote_port).  For listeners that haven't
 * been connected yet, remote_port = 0 acts as a wildcard.
 *
 * Phase plan (this header is locked in across all phases; only the
 * handler implementations grow):
 *   T1a   module registered, bind/unbind handshake to IP.  Other
 *         primitives return T_DISCON_IND with reason=NOTSUP.
 *   T1b   active open: T_CONN_REQ -> SYN -> SYN-ACK -> ACK ->
 *         ESTABLISHED + T_CONN_CON
 *   T1c   data send/recv: T_DATA_REQ / T_DATA_IND with sequence /
 *         ack tracking; no retransmit yet (rely on lo0's reliability
 *         for self-tests)
 *   T1d   passive open: T_CONN_IND / T_CONN_RES, accept queue
 *   T1e   graceful close (FIN)
 *   T1f   retransmit timer + RTT estimation
 *   T1g   cmd/tcptest end-to-end exercise via /dev/tcp
 */

#ifndef KAPPARA_NET_TCP_H
#define KAPPARA_NET_TCP_H

#include <stdint.h>

struct streamtab;
struct queue;
struct msgb;
typedef struct msgb mblk_t;

#define TCP_HDR_LEN_MIN	20	/* no options */

/* T1l: SYN-only option negotiation.  Three kinds we care about
 * (RFC 793 + RFC 1122): END=0 terminates the list, NOP=1 is a
 * single-byte padder, MSS=2 is a 4-byte (kind, len, mss_hi, mss_lo)
 * tuple.  We currently emit only MSS on SYN/SYN-ACK; subsequent
 * segments carry no options. */
#define TCP_OPT_END	0
#define TCP_OPT_NOP	1
#define TCP_OPT_MSS	2
#define TCP_OPT_MSS_LEN	4

/* T1m: window scale (RFC 7323 sec 2).  Shift value 0..14; values >14
 * MUST be clamped per the RFC.  Negotiated on SYN/SYN-ACK only;
 * applies to every subsequent segment's 16-bit window field. */
#define TCP_OPT_WSCALE		3
#define TCP_OPT_WSCALE_LEN	3
#define TCP_WSCALE_MAX		14
#define TCP_LOCAL_WSCALE	0	/* our advertised window <= 16 bits */

/* On-wire TCP header (RFC 793).  All big-endian. */
struct tcp_hdr {
	uint16_t src_port;
	uint16_t dst_port;
	uint32_t seq;
	uint32_t ack;
	uint8_t  data_off;	/* high 4 bits = header length in 32-bit words */
	uint8_t  flags;
	uint16_t window;
	uint16_t checksum;
	uint16_t urg_ptr;
} __attribute__((packed));

#define TCP_FLAG_FIN	0x01
#define TCP_FLAG_SYN	0x02
#define TCP_FLAG_RST	0x04
#define TCP_FLAG_PSH	0x08
#define TCP_FLAG_ACK	0x10
#define TCP_FLAG_URG	0x20

/* ---- TPI primitives ----------------------------------------------- */

#define T_TCP_BIND_REQ		0x10	/* user -> kern: bind local port  */
#define T_TCP_BIND_ACK		0x11
#define T_TCP_BIND_NAK		0x12
#define T_TCP_CONN_REQ		0x13	/* user -> kern: active open       */
#define T_TCP_CONN_CON		0x14	/* kern -> user: handshake done    */
#define T_TCP_CONN_IND		0x15	/* kern -> user: incoming SYN      */
#define T_TCP_CONN_RES		0x16	/* user -> kern: accept the conn   */
#define T_TCP_DATA_REQ		0x17	/* user -> kern + M_DATA payload   */
#define T_TCP_DATA_IND		0x18	/* kern -> user + M_DATA payload   */
#define T_TCP_ORDREL_REQ	0x19	/* user -> kern: send FIN          */
#define T_TCP_ORDREL_IND	0x1a	/* kern -> user: peer FIN'd        */
#define T_TCP_DISCON_REQ	0x1b	/* user -> kern: abort (RST)       */
#define T_TCP_DISCON_IND	0x1c	/* kern -> user: connection aborted */

#define TCP_DISCON_REASON_PROTOERR	1	/* bad TPI sequencing       */
#define TCP_DISCON_REASON_NOTSUP	2	/* primitive not yet wired */
#define TCP_DISCON_REASON_RESET		3	/* peer sent RST            */
#define TCP_DISCON_REASON_TIMEOUT	4	/* SYN/data retransmit lim  */

struct t_tcp_bind_req {
	uint8_t  prim;
	uint8_t  _pad[1];
	uint16_t port;		/* host byte order; 0 = ephemeral */
};

struct t_tcp_bind_ack {
	uint8_t  prim;
	uint8_t  _pad[1];
	uint16_t port;		/* the port we bound (may be ephemeral) */
};

struct t_tcp_bind_nak {
	uint8_t  prim;
	uint8_t  reason;
	uint8_t  _pad[2];
};

struct t_tcp_conn_req {
	uint8_t  prim;
	uint8_t  _pad[3];
	uint32_t dst_ip;	/* host byte order */
	uint16_t dst_port;
	uint16_t _pad2;
};

struct t_tcp_conn_con {
	uint8_t  prim;
	uint8_t  _pad[3];
	uint32_t dst_ip;
	uint16_t dst_port;
	uint16_t _pad2;
};

struct t_tcp_conn_ind {
	uint8_t  prim;
	uint8_t  _pad[3];
	uint32_t src_ip;
	uint16_t src_port;
	uint16_t _pad2;
};

/* T1d.5 multi-accept: T_CONN_RES carries the file descriptor of a
 * fresh /dev/tcp endpoint that will receive the accepted connection.
 * The listener stays in LISTEN; the kernel pops the head pending
 * SYN from the listener's backlog, populates that responding endpoint
 * with the connection state, and sends SYN-ACK from it.  T_CONN_CON
 * fires on the responding fd (not the listener).
 *
 * The responding endpoint must be a freshly-opened /dev/tcp in CLOSED
 * state (no bind / connect / listen yet).  Sharing local_port with the
 * listener is implicit -- the child filters segments by 4-tuple and
 * is routed to via fan-out from the listener's TCB. */
struct t_tcp_conn_res {
	uint8_t  prim;
	uint8_t  _pad[3];
	int32_t  responding_fd;
};

/* T_TCP_LISTEN_REQ: explicit BOUND -> LISTEN transition.  Without
 * it a bound TCB silently drops inbound SYNs.  T1d split this from
 * the T1b/T1c implicit-listener hack where BOUND auto-accepted. */
struct t_tcp_listen_req {
	uint8_t  prim;
	uint8_t  _pad[3];
};

#define T_TCP_LISTEN_REQ	0x1d

struct t_tcp_data_req {
	uint8_t  prim;
	uint8_t  _pad[3];
	/* M_DATA payload chained as b_cont */
};

struct t_tcp_data_ind {
	uint8_t  prim;
	uint8_t  _pad[3];
	/* M_DATA payload chained as b_cont */
};

struct t_tcp_ordrel_req {
	uint8_t  prim;
	uint8_t  _pad[3];
};

struct t_tcp_ordrel_ind {
	uint8_t  prim;
	uint8_t  _pad[3];
};

struct t_tcp_discon_req {
	uint8_t  prim;
	uint8_t  _pad[3];
};

struct t_tcp_discon_ind {
	uint8_t  prim;
	uint8_t  reason;
	uint8_t  _pad[2];
};

/* Streamtab + boot-time registry hookup. */
extern struct streamtab tcp_streamtab;
void tcp_module_init(void);
void tcp_init_late(void);

/* Per-TCB snapshot exposed by /proc/tcp.  Strings/integers only --
 * no kernel pointers, no mblks.  Used by cmd/netstats. */
struct tcp_tcb_view {
	const char *state_name;
	uint16_t    local_port;
	uint32_t    remote_ip;	/* host byte order; 0 if unconnected */
	uint16_t    remote_port;
	uint32_t    snd_buf_len;
	uint32_t    rto_ms;	/* current RTO in milliseconds */
	uint32_t    srtt_ms;	/* smoothed RTT in milliseconds */
	uint16_t    backlog_depth;	/* LISTEN only: pending-SYN count */
	uint32_t    snd_wnd;	/* peer's last advertised receive window (scaled) */
	uint16_t    rcv_wnd;	/* what we'd advertise right now    */
	uint32_t    cwnd;	/* T1j: congestion window in bytes  */
	uint32_t    ssthresh;	/* T1j: slow-start threshold        */
	uint8_t     dup_acks;	/* T1k: current consecutive-dup-ACK count */
	uint8_t     snd_wnd_shift;	/* T1m: peer's WS shift (0 = no scaling) */
	uint8_t     ws_active;		/* T1m: WS negotiation succeeded */
};

void tcp_for_each_tcb(void (*cb)(const struct tcp_tcb_view *v, void *arg),
                      void *arg);

/* Self-test: opens a tcp stream in-kernel, sends T_BIND_REQ, checks
 * for T_BIND_ACK.  Other primitives get added to the selftest as
 * subsequent phases fill them in.  Returns 0 on PASS, -1 on FAIL. */
int  tcp_selftest_run(void);

#endif

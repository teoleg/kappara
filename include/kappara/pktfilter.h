/*
 * include/kappara/pktfilter.h -- demo pushable STREAMS filter
 *
 * pktfilter is the proof-of-composability piece for the N2 network
 * stack rewrite: a STREAMS module that can be I_PUSHed onto an open
 * /dev/udp (or any TPI-shaped stream) at runtime to inspect outbound
 * M_PROTO + M_DATA messages and drop ones that match a configurable
 * rule.  No autopush -- user-driven by design; the point is to show
 * that the layering is genuinely modular and a third party (or here,
 * a debug module) slots in cleanly.
 *
 * Where it sits
 * -------------
 * I_PUSH on a stream always inserts a module between the stream head
 * and whatever module/driver was at the top.  For an open of
 * /dev/udp the stack starts as
 *
 *     head -> udp -> ip
 *
 * and `ioctl(fd, I_PUSH, "pktfilter")` gives
 *
 *     head -> pktfilter -> udp -> ip
 *
 * pktfilter therefore sees the user's M_PROTO traffic on the way DOWN
 * BEFORE udp processes it.  It does not see the IP_T_SEND_REQ that
 * udp later builds (that crosses the udp/ip boundary below pktfilter).
 * Inbound packets bypass pktfilter entirely because IP demux uses
 * cross-stream put() straight into udp's read queue -- they never
 * traverse the stream-head stack above udp.  Inbound filtering needs
 * a different hook (a future commit can expose ip_hook_input or push
 * pktfilter on the IP control stream itself).
 *
 * Configuration ABI (M_PROTO tagged-union, same shape as TPI)
 * ----------------------------------------------------------
 *   PF_T_SET_REQ   set the drop rule
 *   PF_T_SET_ACK   rule installed
 *   PF_T_STATS_REQ ask for counters
 *   PF_T_STATS_RES counters returned
 *
 * The drop rule is a tuple (enabled, drop_dst_ip, drop_dst_port);
 * a field of 0 acts as a wildcard.  Match on T_UNITDATA_REQ:
 *   drop iff   enabled
 *           && (drop_dst_ip   == 0 || dst_ip   == drop_dst_ip)
 *           && (drop_dst_port == 0 || dst_port == drop_dst_port).
 *
 * Other M_PROTO primitives (T_BIND_REQ, T_BIND_ACK / NAK, etc) pass
 * through unconditionally so the bind/unbind handshake to UDP keeps
 * working regardless of the filter state.
 */

#ifndef KAPPARA_PKTFILTER_H
#define KAPPARA_PKTFILTER_H

#include <stdint.h>

struct streamtab;

#define PF_T_SET_REQ	1
#define PF_T_SET_ACK	2
#define PF_T_STATS_REQ	3
#define PF_T_STATS_RES	4

struct pf_set_req {
	uint8_t  prim;		/* PF_T_SET_REQ */
	uint8_t  enabled;	/* 0 = bypass, 1 = active */
	uint8_t  _pad[2];
	uint32_t drop_dst_ip;	/* host byte order; 0 = wildcard */
	uint16_t drop_dst_port;	/* host byte order; 0 = wildcard */
	uint16_t _pad2;
};

struct pf_set_ack {
	uint8_t  prim;		/* PF_T_SET_ACK */
	uint8_t  _pad[3];
};

struct pf_stats_req {
	uint8_t  prim;		/* PF_T_STATS_REQ */
	uint8_t  _pad[3];
};

struct pf_stats_res {
	uint8_t  prim;		/* PF_T_STATS_RES */
	uint8_t  _pad[3];
	uint32_t passed;
	uint32_t dropped;
};

extern struct streamtab pktfilter_streamtab;

void pktfilter_module_init(void);

#endif

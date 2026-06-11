/*
 * cmd/pktfilttest.c -- prove composability: I_PUSH pktfilter onto an
 *                      open /dev/udp at runtime, watch it drop matching
 *                      datagrams, I_POP it, watch traffic flow again.
 *
 * The point of this test isn't UDP -- it's that a STREAMS module
 * written long after the stack was built can slot in cleanly between
 * the stream head and udp, observe + drop + count outbound traffic,
 * and be removed without leaking state.  That's the property we set
 * out to deliver when we did the full N2 STREAMS rewrite.
 *
 * Sequence:
 *   1. open /dev/udp, bind port 12345
 *   2. send + receive one datagram to 127.0.0.1:12345 -- baseline OK
 *   3. ioctl(I_PUSH, "pktfilter")
 *   4. configure filter to drop dst_port=12345
 *   5. send a datagram -- should be dropped before reaching udp
 *   6. query pktfilter stats -- expect dropped=1
 *   7. ioctl(I_POP, 0)
 *   8. send + receive again -- traffic flows once more, proving the
 *      pop fully unwound the filter's hook on the wput chain
 */

#include <stdint.h>
#include <stdio.h>
#include <stropts.h>
#include <string.h>
#include <unistd.h>
#include "kappara/net/pktfilter.h"
#include "kappara/net/udp.h"

static int bind_port(int fd, uint16_t port)
{
	struct t_bind_req req = { .prim = T_BIND_REQ, .port = port };
	struct strbuf ctl = { .maxlen = 0, .len = sizeof(req), .buf = &req };
	if (putmsg(fd, &ctl, NULL, 0) < 0) return -1;

	struct t_bind_ack ack;
	struct strbuf ctlin = {
		.maxlen = sizeof(ack), .len = 0, .buf = &ack,
	};
	int flags = 0;
	if (getmsg(fd, &ctlin, NULL, &flags) < 0) return -1;
	if (ack.prim != T_BIND_ACK) return -1;
	return ack.port;
}

static int send_unitdata(int fd, uint32_t dst_ip, uint16_t dst_port,
                         const void *payload, unsigned len)
{
	struct t_unitdata_req req = {
		.prim     = T_UNITDATA_REQ,
		.dst_ip   = dst_ip,
		.dst_port = dst_port,
	};
	struct strbuf ctl  = { .maxlen = 0, .len = sizeof(req), .buf = &req };
	struct strbuf data = {
		.maxlen = 0, .len = (int)len, .buf = (void *)payload,
	};
	return putmsg(fd, &ctl, &data, 0);
}

static int recv_unitdata_or_ack(int fd, void *ctl_buf, int ctl_max,
                                void *data_buf, int data_max)
{
	struct strbuf ctl  = { .maxlen = ctl_max,  .len = 0, .buf = ctl_buf };
	struct strbuf data = { .maxlen = data_max, .len = 0, .buf = data_buf };
	int flags = 0;
	int r = getmsg(fd, &ctl, &data, &flags);
	if (r < 0) return -1;
	return data.len;
}

static int query_stats(int fd, struct pf_stats_res *out)
{
	struct pf_stats_req req = { .prim = PF_T_STATS_REQ };
	struct strbuf ctl = { .maxlen = 0, .len = sizeof(req), .buf = &req };
	if (putmsg(fd, &ctl, NULL, 0) < 0) return -1;

	struct strbuf ctlin = {
		.maxlen = sizeof(*out), .len = 0, .buf = out,
	};
	int flags = 0;
	if (getmsg(fd, &ctlin, NULL, &flags) < 0) return -1;
	if (out->prim != PF_T_STATS_RES) return -1;
	return 0;
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	int fd = open("/dev/udp", 2);
	if (fd < 0) { puts("pktfilttest: open /dev/udp failed"); return 1; }

	int port = bind_port(fd, 12345);
	if (port < 0) { puts("pktfilttest: bind failed"); close(fd); return 1; }
	printf("pktfilttest: bound port %d\n", port);

	const char payload[] = "ping";
	const unsigned plen  = sizeof(payload) - 1;

	/* Baseline: send + receive. */
	if (send_unitdata(fd, 0x7f000001u, (uint16_t)port,
	                  payload, plen) < 0) {
		puts("pktfilttest: send (baseline) failed");
		close(fd); return 1;
	}
	{
		struct t_unitdata_ind ind;
		char buf[64];
		int n = recv_unitdata_or_ack(fd, &ind, sizeof(ind),
		                             buf, sizeof(buf));
		if (n != (int)plen) {
			puts("pktfilttest: baseline echo missing");
			close(fd); return 1;
		}
	}
	puts("pktfilttest: baseline OK");

	/* Push pktfilter. */
	if (ioctl(fd, I_PUSH, (long)(unsigned long)"pktfilter") < 0) {
		puts("pktfilttest: I_PUSH pktfilter failed");
		close(fd); return 1;
	}
	puts("pktfilttest: pktfilter pushed");

	/* Configure: drop dst_port=12345, any IP. */
	{
		struct pf_set_req req = {
			.prim          = PF_T_SET_REQ,
			.enabled       = 1,
			.drop_dst_ip   = 0,
			.drop_dst_port = (uint16_t)port,
		};
		struct strbuf ctl = {
			.maxlen = 0, .len = sizeof(req), .buf = &req,
		};
		if (putmsg(fd, &ctl, NULL, 0) < 0) {
			puts("pktfilttest: PF_T_SET_REQ failed");
			close(fd); return 1;
		}
		struct pf_set_ack ack;
		struct strbuf ctlin = {
			.maxlen = sizeof(ack), .len = 0, .buf = &ack,
		};
		int flags = 0;
		if (getmsg(fd, &ctlin, NULL, &flags) < 0
		    || ack.prim != PF_T_SET_ACK) {
			puts("pktfilttest: PF_T_SET_ACK missing");
			close(fd); return 1;
		}
	}

	/* Send a packet -- pktfilter should drop it.  We don't getmsg
	 * for it (none will come); we query stats instead. */
	if (send_unitdata(fd, 0x7f000001u, (uint16_t)port,
	                  payload, plen) < 0) {
		puts("pktfilttest: drop-send returned error");
		close(fd); return 1;
	}

	struct pf_stats_res stats;
	if (query_stats(fd, &stats) < 0) {
		puts("pktfilttest: stats query failed");
		close(fd); return 1;
	}
	printf("pktfilttest: stats passed=%u dropped=%u\n",
	       (unsigned)stats.passed, (unsigned)stats.dropped);
	if (stats.dropped != 1) {
		puts("pktfilttest: expected dropped=1");
		close(fd); return 1;
	}

	/* Pop the filter -- traffic should flow again. */
	if (ioctl(fd, I_POP, 0) < 0) {
		puts("pktfilttest: I_POP failed");
		close(fd); return 1;
	}
	puts("pktfilttest: pktfilter popped");

	if (send_unitdata(fd, 0x7f000001u, (uint16_t)port,
	                  payload, plen) < 0) {
		puts("pktfilttest: post-pop send failed");
		close(fd); return 1;
	}
	{
		struct t_unitdata_ind ind;
		char buf[64];
		int n = recv_unitdata_or_ack(fd, &ind, sizeof(ind),
		                             buf, sizeof(buf));
		if (n != (int)plen) {
			puts("pktfilttest: post-pop echo missing");
			close(fd); return 1;
		}
	}

	puts("pktfilttest: PASS");
	close(fd);
	return 0;
}

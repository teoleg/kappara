/*
 * include/kappara/net/slip.h -- SLIP framing module + boot wiring
 *
 * SLIP (RFC 1055) over the BCM2837 mini-UART, structured exactly the
 * way Solaris's slattach attaches a SLIP line at boot:
 *
 *     stream head                                  ─┐
 *         |                                         │
 *     slip module     (qopen / qclose / wput / rput)│  one stream that
 *         |                                         │  IS the slip0
 *     miniuart leaf driver  (byte-stream to wire)   │  netif from IP's
 *         |                                         │  POV after I_LINK
 *     UART hardware                                ─┘
 *
 * slip_init() at boot:
 *   1. miniuart_init(115200)
 *   2. stream_build_kernel(&slip_lower_streamtab) -- the byte stream
 *   3. stream_push_kernel(sd, "slip")             -- frame layer
 *   4. spawn the mini-UART rx kthread, hand it sd->sd_drv_rq so
 *      every received byte is allocb'd + putnext'd up into SLIP
 *   5. netif_register(slip0)                       -- name/IP/mask
 *   6. ip_attach_stream(sd, slip0)                 -- I_LINKed under IP
 *
 * Default configuration (compile-time):
 *   slip0 IP      = 192.168.10.2
 *   slip0 netmask = 255.255.255.252  (/30)
 *   peer (host)   = 192.168.10.1
 *   MTU           = 296 bytes (RFC 1055 recommendation)
 *
 * Host-side smoke test on Linux:
 *   socat PTY,raw,echo=0,link=/tmp/kappara-uart1 \
 *         QEMU's chardev pipe  &
 *   slattach -L -p slip -s 115200 /tmp/kappara-uart1 &
 *   ifconfig sl0 192.168.10.1 pointopoint 192.168.10.2 up
 *   ping 192.168.10.2
 */

#ifndef KAPPARA_NET_SLIP_H
#define KAPPARA_NET_SLIP_H

struct streamtab;

extern struct streamtab slip_streamtab;

/* Boot wiring: brings up mini-UART, builds the slip-over-uart stream,
 * registers slip0 as a netif, and I_LINKs it under IP.  Must run
 * AFTER ip_init (needs the IP control stream to attach to). */
void slip_init(void);

/* Self-test stub: returns 0 (nothing meaningful to test in-kernel
 * without a peer driving the wire).  Real test is host-side
 * ping/UDP through slattach. */
int  slip_selftest_run(void);

/* Register the slip STREAMS module by name so I_PUSH "slip" can
 * find it.  Called from streams_head_init. */
void slip_module_init(void);

#endif

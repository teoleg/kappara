/*
 * uts/virt/virtio_net.c -- virtio-mmio + virtio-net driver (QEMU virt)
 *
 * Phase 2 of the road to telnet-from-anywhere: bring up eth0 on the
 * QEMU virt machine so IP has an interface other than lo0.
 *
 * This commit lands the virtio-mmio probe + ring bring-up + RX/TX
 * plumbing.  The IP integration (ip_attach_stream'ing eth0 under
 * /dev/ip so packets actually route) is the next commit -- once the
 * MMIO + ring path is proven solid against QEMU's virtio-net model,
 * the streamtab/I_LINK wiring follows the slip0 shape exactly.
 *
 * Layout (virtio v1.1 + virtio-mmio v2):
 *
 *   Device-config MMIO at 0x0A000000 + 0x200 * slot.
 *
 *   Selected registers we touch:
 *     0x000 Magic ("virt" LE = 0x74726976)
 *     0x004 Version (2 = modern)
 *     0x008 DeviceID (1 = net)
 *     0x010/0x014 DeviceFeatures + Sel
 *     0x020/0x024 DriverFeatures + Sel
 *     0x030 QueueSel
 *     0x034 QueueNumMax
 *     0x038 QueueNum
 *     0x044 QueueReady
 *     0x050 QueueNotify
 *     0x060/0x064 InterruptStatus + ACK
 *     0x070 Status
 *     0x080..0x0A8 Queue ring physical addresses
 *     0x100+ device config (net: MAC, link status)
 *
 * IRQ wiring: virt assigns virtio-mmio slot N to GIC SPI (16 + N);
 * the GIC INTID is (32 + 16 + N) = 48 for slot 0.  Enable via
 * gic_enable_irq + dispatch from irq_dispatch.
 */

#include <stdint.h>

#include "kappara/arch/gic.h"
#include "kappara/core/kmem.h"
#include "kappara/core/pmm.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"
#include "kappara/io/streams.h"
#include "kappara/io/stream_head.h"
#include "kappara/net/arp.h"
#include "kappara/net/ip.h"
#include "kappara/net/netif.h"
#include "kappara/proc/sched.h"
#include "platform.h"

/* ---- MMIO register offsets -------------------------------------- */

#define VMMIO_MAGIC		0x000
#define VMMIO_VERSION		0x004
#define VMMIO_DEVICE_ID		0x008
#define VMMIO_VENDOR_ID		0x00C
#define VMMIO_DEV_FEAT		0x010
#define VMMIO_DEV_FEAT_SEL	0x014
#define VMMIO_DRV_FEAT		0x020
#define VMMIO_DRV_FEAT_SEL	0x024
#define VMMIO_QSEL		0x030
#define VMMIO_QNUM_MAX		0x034
#define VMMIO_QNUM		0x038
#define VMMIO_QREADY		0x044
#define VMMIO_QNOTIFY		0x050
#define VMMIO_INT_STATUS	0x060
#define VMMIO_INT_ACK		0x064
#define VMMIO_STATUS		0x070
#define VMMIO_QDESC_LO		0x080
#define VMMIO_QDESC_HI		0x084
#define VMMIO_QDRV_LO		0x090
#define VMMIO_QDRV_HI		0x094
#define VMMIO_QDEV_LO		0x0A0
#define VMMIO_QDEV_HI		0x0A4
#define VMMIO_CONFIG		0x100

#define VIRTIO_MAGIC		0x74726976
#define VIRTIO_DEV_NET		1

#define VS_ACKNOWLEDGE		1
#define VS_DRIVER		2
#define VS_DRIVER_OK		4
#define VS_FEATURES_OK		8
#define VS_FAILED		0x80

#define VNET_F_MAC		(1u << 5)
#define VNET_F_STATUS		(1u << 16)

struct virtio_net_hdr {
	uint8_t  flags;
	uint8_t  gso_type;
	uint16_t hdr_len;
	uint16_t gso_size;
	uint16_t csum_start;
	uint16_t csum_offset;
} __attribute__((packed));

#define VNET_HDR_LEN	sizeof(struct virtio_net_hdr)

/* ---- Ethernet + ARP --------------------------------------------- */

struct ether_hdr {
	uint8_t  dst[6];
	uint8_t  src[6];
	uint16_t ethertype;	/* network byte order */
} __attribute__((packed));

#define ETH_HDR_LEN	14
#define ETHERTYPE_IPV4	0x0800
#define ETHERTYPE_ARP	0x0806

/* eth0's IPv4 config.  Until DHCP completes eth0_ip / NETMASK /
 * GATEWAY are zero (we listen for the DHCPOFFER as a raw-Ethernet
 * broadcast so we don't need an IP yet).  The compile-time fallbacks
 * (FALLBACK_*) come into play if DHCP times out without an OFFER --
 * they keep the historic well-known QEMU slirp values so the box
 * still boots into a usable shell on a network that has no DHCP
 * server.  See dhcp_run below for the state machine. */
static uint32_t eth0_ip;
static uint32_t eth0_netmask;
static uint32_t eth0_gateway;

#define FALLBACK_IP		0x0a00020fu	/* 10.0.2.15 */
#define FALLBACK_NETMASK	0xffffff00u	/* /24 */
#define FALLBACK_GATEWAY	0x0a000202u	/* 10.0.2.2 */

/* ARP moved to the shared cache (uts/os/net/arp.c, /proc/arp).
 * eth0_arpif carries our identity + raw TX into it. */
static struct arpif eth0_arpif;

/* Forward decls so rx_dispatch can hand IPv4 frames to the DHCP
 * filter before the IP layer comes up.  Definitions below. */
static int dhcp_rx_filter(const uint8_t *frame, unsigned len);

/* ---- Virtqueue rings ------------------------------------------- */

#define VQ_NUM	64

struct vring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};
#define VDESC_NEXT	(1u << 0)
#define VDESC_WRITE	(1u << 1)

struct vring_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[VQ_NUM];
	uint16_t used_event;
} __attribute__((packed));

struct vring_used_elem {
	uint32_t id;
	uint32_t len;
};
struct vring_used {
	uint16_t flags;
	uint16_t idx;
	struct vring_used_elem ring[VQ_NUM];
	uint16_t avail_event;
} __attribute__((packed));

struct vqueue {
	struct vring_desc  *desc;
	struct vring_avail *avail;
	struct vring_used  *used;
	uint16_t            num;
	uint16_t            free_head;
	uint16_t            last_used;
	uint16_t            qsel;
};

#define RXQ	0
#define TXQ	1

static struct {
	uintptr_t     mmio_base;
	unsigned      slot;
	unsigned      intid;
	uint8_t       mac[6];
	struct vqueue rx;
	struct vqueue tx;
	struct netif  nif;
	void         *rx_bufs[VQ_NUM];
	void         *tx_hdrs[VQ_NUM];
	int           ready;
} g_eth0;

static inline uint32_t mr32(unsigned off)
{
	return *(volatile uint32_t *)(g_eth0.mmio_base + off);
}
static inline void mw32(unsigned off, uint32_t val)
{
	*(volatile uint32_t *)(g_eth0.mmio_base + off) = val;
}

static int vq_init(struct vqueue *q, unsigned sel)
{
	q->qsel       = (uint16_t)sel;
	q->last_used  = 0;
	q->free_head  = 0;

	mw32(VMMIO_QSEL, sel);
	uint32_t qmax = mr32(VMMIO_QNUM_MAX);
	if (qmax == 0) {
		kprintf("virtio-net: queue %u not supported\n", sel);
		return -1;
	}
	q->num = (qmax < VQ_NUM) ? (uint16_t)qmax : VQ_NUM;
	mw32(VMMIO_QNUM, q->num);

	void *desc_pg  = pmm_alloc();
	void *avail_pg = pmm_alloc();
	void *used_pg  = pmm_alloc();
	if (!desc_pg || !avail_pg || !used_pg) {
		kprintf("virtio-net: pmm_alloc failed for queue %u\n", sel);
		return -1;
	}
	kmemset(desc_pg,  0, PAGE_SIZE);
	kmemset(avail_pg, 0, PAGE_SIZE);
	kmemset(used_pg,  0, PAGE_SIZE);

	q->desc  = desc_pg;
	q->avail = avail_pg;
	q->used  = used_pg;

	uint64_t da = (uint64_t)(uintptr_t)desc_pg;
	uint64_t aa = (uint64_t)(uintptr_t)avail_pg;
	uint64_t ua = (uint64_t)(uintptr_t)used_pg;
	mw32(VMMIO_QDESC_LO, (uint32_t)da);
	mw32(VMMIO_QDESC_HI, (uint32_t)(da >> 32));
	mw32(VMMIO_QDRV_LO,  (uint32_t)aa);
	mw32(VMMIO_QDRV_HI,  (uint32_t)(aa >> 32));
	mw32(VMMIO_QDEV_LO,  (uint32_t)ua);
	mw32(VMMIO_QDEV_HI,  (uint32_t)(ua >> 32));
	mw32(VMMIO_QREADY, 1);
	return 0;
}

/* Hand an RX slot back to the device. */
static void rx_post(unsigned slot)
{
	struct vring_desc *d = &g_eth0.rx.desc[slot];
	d->addr  = (uint64_t)(uintptr_t)g_eth0.rx_bufs[slot];
	d->len   = PAGE_SIZE;
	d->flags = VDESC_WRITE;
	d->next  = 0;

	uint16_t idx = g_eth0.rx.avail->idx;
	g_eth0.rx.avail->ring[idx % g_eth0.rx.num] = (uint16_t)slot;
	__asm__ volatile ("dmb sy" ::: "memory");
	g_eth0.rx.avail->idx = idx + 1;
	__asm__ volatile ("dmb sy" ::: "memory");
}

static int rx_prepare(void)
{
	for (unsigned i = 0; i < g_eth0.rx.num; i++) {
		g_eth0.rx_bufs[i] = pmm_alloc();
		if (!g_eth0.rx_bufs[i]) return -1;
		rx_post(i);
	}
	mw32(VMMIO_QNOTIFY, RXQ);
	return 0;
}

static int tx_prepare(void)
{
	for (unsigned i = 0; i < g_eth0.tx.num; i++) {
		g_eth0.tx_hdrs[i] = kmalloc(VNET_HDR_LEN);
		if (!g_eth0.tx_hdrs[i]) return -1;
		kmemset(g_eth0.tx_hdrs[i], 0, VNET_HDR_LEN);
	}
	return 0;
}

/* TX: build a 2-desc chain (header + payload), kick.  Caller passes
 * a flat buffer + length already containing the full Ethernet frame
 * (Ethernet header + payload). */
int virtio_net_tx_bytes(const void *buf, unsigned len)
{
	if (!g_eth0.ready || len == 0 || len > PAGE_SIZE) return -1;

	void *payload = pmm_alloc();
	if (!payload) return -1;
	kmemcpy(payload, buf, len);

	uint16_t hslot = g_eth0.tx.free_head;
	uint16_t pslot = (uint16_t)((hslot + 1) % g_eth0.tx.num);
	g_eth0.tx.free_head = (uint16_t)((hslot + 2) % g_eth0.tx.num);

	struct vring_desc *h = &g_eth0.tx.desc[hslot];
	h->addr  = (uint64_t)(uintptr_t)g_eth0.tx_hdrs[hslot];
	h->len   = VNET_HDR_LEN;
	h->flags = VDESC_NEXT;
	h->next  = pslot;

	struct vring_desc *d = &g_eth0.tx.desc[pslot];
	d->addr  = (uint64_t)(uintptr_t)payload;
	d->len   = len;
	d->flags = 0;
	d->next  = 0;

	uint16_t idx = g_eth0.tx.avail->idx;
	g_eth0.tx.avail->ring[idx % g_eth0.tx.num] = hslot;
	__asm__ volatile ("dmb sy" ::: "memory");
	g_eth0.tx.avail->idx = idx + 1;
	__asm__ volatile ("dmb sy" ::: "memory");
	mw32(VMMIO_QNOTIFY, TXQ);
	return 0;
}

/* ---- ARP + Ethernet helpers ------------------------------------ */

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xff);
}

/* Trampoline so the shared ARP module can transmit raw frames. */
static int eth0_arp_tx(void *cookie, const void *frame, unsigned len)
{
	(void)cookie;
	return virtio_net_tx_bytes(frame, len);
}

/* Forward declaration: queue_t to attach to RX delivery path. */
static struct stdata *eth0_sd;

static void rx_dispatch(const uint8_t *frame, unsigned len)
{
	if (len < ETH_HDR_LEN) return;
	const struct ether_hdr *eh = (const struct ether_hdr *)frame;
	uint16_t et = (uint16_t)(((eh->ethertype & 0xff) << 8)
	                         | ((eh->ethertype >> 8) & 0xff));

	if (et == ETHERTYPE_ARP) {
		arp_input(&eth0_arpif, frame, len);
		return;
	}

	if (et == ETHERTYPE_IPV4) {
		/* Dhcp state machine wants first crack at UDP/68 frames
		 * because the IP layer isn't wired up yet during the
		 * pre-IP-attach DHCP exchange.  After DHCP_BOUND this
		 * filter is a no-op. */
		if (dhcp_rx_filter(frame, len)) return;
	}

	if (et == ETHERTYPE_IPV4 && eth0_sd && eth0_sd->sd_rq) {
		/* Strip Ethernet header, send the IP datagram up to the
		 * IP mux via the netif stream's read side. */
		unsigned plen = len - ETH_HDR_LEN;
		mblk_t *mp = allocb(plen, 0);
		if (mp) {
			kmemcpy(mp->b_wptr, frame + ETH_HDR_LEN, plen);
			mp->b_wptr += plen;
			mp->b_datap->db_type = M_DATA;
			/* Put up the stream's read-side chain so ip_rput
			 * sees it. */
			queue_t *rq = eth0_sd->sd_drv_rq;
			if (rq && rq->q_next)
				putnext(rq, mp);
			else
				freemsg(mp);
		}
	}
}

/* IRQ entry: drain used rings. */
void virtio_net_irq(void)
{
	if (!g_eth0.ready) return;

	uint32_t status = mr32(VMMIO_INT_STATUS);
	mw32(VMMIO_INT_ACK, status);

	while (g_eth0.rx.last_used != g_eth0.rx.used->idx) {
		struct vring_used_elem e;
		kmemcpy(&e,
		    &g_eth0.rx.used->ring[g_eth0.rx.last_used % g_eth0.rx.num],
		    sizeof(e));
		unsigned slot = (unsigned)e.id;
		unsigned len  = (unsigned)e.len;
		g_eth0.rx.last_used++;
		if (len > VNET_HDR_LEN && len <= PAGE_SIZE) {
			rx_dispatch((uint8_t *)g_eth0.rx_bufs[slot]
			            + VNET_HDR_LEN,
			            len - VNET_HDR_LEN);
		}
		rx_post(slot);
	}
	mw32(VMMIO_QNOTIFY, RXQ);
}

/* ---- eth0 streamtab + ip_attach_stream -------------------------- */
/*
 * Write side: IP wput sends an mblk containing a raw IP datagram.
 * We prepend an Ethernet header (dst = cached gateway MAC, src = our
 * MAC, ethertype = IPv4) and TX through virtio.
 */
static int eth0_wq_putp(queue_t *q, mblk_t *mp)
{
	(void)q;
	if (!mp) return 0;
	if (mp->b_datap->db_type != M_DATA) { freemsg(mp); return 0; }

	unsigned plen = (unsigned)msgdsize(mp);
	if (plen == 0 || plen > PAGE_SIZE - ETH_HDR_LEN) {
		freemsg(mp);
		return -1;
	}

	/* Next hop: on-subnet destinations get ARPed directly, anything
	 * else goes via the gateway.  The datagram's dst lives at bytes
	 * 16..19 of the IP header (ip_wput builds it contiguously). */
	uint8_t dmac[6];
	{
		const uint8_t *iph = mp->b_rptr;
		uint32_t dst = ((uint32_t)iph[16] << 24) | ((uint32_t)iph[17] << 16)
		             | ((uint32_t)iph[18] <<  8) |  (uint32_t)iph[19];
		if (dst == 0xffffffffu) {
			kmemset(dmac, 0xff, 6);
		} else {
			uint32_t nh = ((dst & eth0_netmask) ==
			               (eth0_ip & eth0_netmask)) ? dst
			                                          : eth0_gateway;
			if (!arp_resolve(&eth0_arpif, nh, dmac)) {
				/* Request is on the wire; drop this packet,
				 * upper layers retransmit. */
				freemsg(mp);
				return 0;
			}
		}
	}

	uint8_t buf[ETH_HDR_LEN + 1500];
	struct ether_hdr *eh = (struct ether_hdr *)buf;
	kmemcpy(eh->dst, dmac, 6);
	kmemcpy(eh->src, g_eth0.mac, 6);
	eh->ethertype = (uint16_t)((ETHERTYPE_IPV4 & 0xff) << 8
	                         | ((ETHERTYPE_IPV4 >> 8) & 0xff));

	unsigned char *p = buf + ETH_HDR_LEN;
	for (mblk_t *m = mp; m; m = m->b_cont) {
		unsigned n = (unsigned)(m->b_wptr - m->b_rptr);
		if (n == 0) continue;
		kmemcpy(p, m->b_rptr, n);
		p += n;
	}
	freemsg(mp);

	virtio_net_tx_bytes(buf, ETH_HDR_LEN + plen);
	return 0;
}

static int eth0_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static int eth0_qopen(queue_t *rq)
{
	(void)rq;
	return 0;
}

static struct module_info eth0_minfo = {
	.mi_idnum  = 2000,
	.mi_idname = "eth0",
	.mi_minpsz = 0,
	.mi_maxpsz = 1500,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit eth0_rinit = {
	.qi_putp  = eth0_rq_putp,
	.qi_qopen = eth0_qopen,
	.qi_minfo = &eth0_minfo,
};
static struct qinit eth0_winit = {
	.qi_putp  = eth0_wq_putp,
	.qi_minfo = &eth0_minfo,
};
struct streamtab eth0_streamtab = {
	.st_rdinit = &eth0_rinit,
	.st_wrinit = &eth0_winit,
};

static struct netif eth0_nif;

/* ---- DHCP client (RFC 2131) ------------------------------------
 *
 * Run before ip_attach_stream so the IP layer comes up with the
 * address slirp (or any other DHCP server on the wire) hands us
 * rather than the hardcoded 10.0.2.15.  Pure raw-Ethernet
 * implementation -- we bypass kappara's IP/UDP stack because (a)
 * the stack hasn't been wired up yet at this point in init, and
 * (b) DHCP needs IP src 0.0.0.0 + dst 255.255.255.255 which our
 * stack would refuse anyway.
 *
 * Sequence: DISCOVER (broadcast) -> wait OFFER -> REQUEST
 * (broadcast) -> wait ACK -> commit yiaddr / netmask / gateway to
 * eth0_nif.  Timeout -> fall back to the historic well-known
 * QEMU slirp values so a no-DHCP-server network still boots.
 */

#define DHCP_OP_BOOTREQUEST	1
#define DHCP_OP_BOOTREPLY	2
#define DHCP_HTYPE_ETHER	1
#define DHCP_HLEN_ETHER		6
#define DHCP_MAGIC		0x63825363u

#define DHCP_MSG_DISCOVER	1
#define DHCP_MSG_OFFER		2
#define DHCP_MSG_REQUEST	3
#define DHCP_MSG_ACK		5

#define DHCP_OPT_PAD		0
#define DHCP_OPT_SUBNET		1
#define DHCP_OPT_ROUTER		3
#define DHCP_OPT_REQ_IP		50
#define DHCP_OPT_MSG_TYPE	53
#define DHCP_OPT_SERVER_ID	54
#define DHCP_OPT_PARAM_LIST	55
#define DHCP_OPT_END		255

#define DHCP_CLIENT_PORT	68
#define DHCP_SERVER_PORT	67

#define IPPROTO_UDP		17
#define IPV4_HDR_LEN		20
#define UDP_HDR_LEN		8

struct dhcp_pkt {
	uint8_t  op;
	uint8_t  htype;
	uint8_t  hlen;
	uint8_t  hops;
	uint32_t xid;		/* network order */
	uint16_t secs;
	uint16_t flags;		/* 0x0080 = broadcast reply */
	uint32_t ciaddr;
	uint32_t yiaddr;
	uint32_t siaddr;
	uint32_t giaddr;
	uint8_t  chaddr[16];
	uint8_t  sname[64];
	uint8_t  file[128];
	uint32_t magic;		/* DHCP_MAGIC network order */
	uint8_t  options[312];	/* RFC 2131 says 312 bytes minimum */
} __attribute__((packed));

enum {
	DHCP_INIT,
	DHCP_WAIT_OFFER,
	DHCP_WAIT_ACK,
	DHCP_BOUND,
	DHCP_FAILED,
};

static struct {
	int      state;
	uint32_t xid;
	uint32_t offer_ip;
	uint32_t offer_netmask;
	uint32_t offer_gw;
	uint32_t offer_server;
	struct wait_queue wq;
} dhcp;

extern uint16_t ip_checksum(const void *buf, unsigned len);

/* UDP checksum over the IPv4 pseudo-header + UDP header + payload.
 * Zero is legal for IPv4 (means "no checksum"); slirp accepts it,
 * but real switches don't necessarily, so we compute it properly. */
static uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t *udp, unsigned udp_len)
{
	uint32_t sum = 0;
	sum += (src_ip >> 16) & 0xffff;
	sum += src_ip & 0xffff;
	sum += (dst_ip >> 16) & 0xffff;
	sum += dst_ip & 0xffff;
	sum += IPPROTO_UDP;
	sum += udp_len;
	for (unsigned i = 0; i + 1 < udp_len; i += 2)
		sum += ((uint16_t)udp[i] << 8) | udp[i + 1];
	if (udp_len & 1)
		sum += (uint16_t)udp[udp_len - 1] << 8;
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >>  8);
	p[3] = (uint8_t) v;
}

static uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
	     | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* Build + send a DHCPDISCOVER or DHCPREQUEST.  Both have the same
 * Ethernet / IP / UDP wrapping; only the DHCP option bytes differ. */
static int dhcp_send(uint8_t msg_type, uint32_t req_ip, uint32_t server_id)
{
	uint8_t frame[ETH_HDR_LEN + IPV4_HDR_LEN + UDP_HDR_LEN
	             + sizeof(struct dhcp_pkt)];
	kmemset(frame, 0, sizeof(frame));

	/* Ethernet header: src = our MAC, dst = ff:ff:ff:ff:ff:ff. */
	struct ether_hdr *eh = (struct ether_hdr *)frame;
	for (int i = 0; i < 6; i++) {
		eh->dst[i] = 0xff;
		eh->src[i] = g_eth0.mac[i];
	}
	put_be16((uint8_t *)&eh->ethertype, ETHERTYPE_IPV4);

	/* DHCP payload sits at offset ETH+IP+UDP. */
	struct dhcp_pkt *d =
	    (struct dhcp_pkt *)(frame + ETH_HDR_LEN + IPV4_HDR_LEN + UDP_HDR_LEN);
	d->op    = DHCP_OP_BOOTREQUEST;
	d->htype = DHCP_HTYPE_ETHER;
	d->hlen  = DHCP_HLEN_ETHER;
	put_be32((uint8_t *)&d->xid, dhcp.xid);
	put_be16((uint8_t *)&d->flags, 0x0000);
	for (int i = 0; i < 6; i++) d->chaddr[i] = g_eth0.mac[i];
	put_be32((uint8_t *)&d->magic, DHCP_MAGIC);

	/* Options: msg type + (optional req IP + server id) + param list
	 * + end. */
	uint8_t *o = d->options;
	*o++ = DHCP_OPT_MSG_TYPE; *o++ = 1; *o++ = msg_type;
	if (msg_type == DHCP_MSG_REQUEST) {
		*o++ = DHCP_OPT_REQ_IP;
		*o++ = 4;
		put_be32(o, req_ip);            o += 4;
		*o++ = DHCP_OPT_SERVER_ID;
		*o++ = 4;
		put_be32(o, server_id);         o += 4;
	}
	*o++ = DHCP_OPT_PARAM_LIST;
	*o++ = 2;
	*o++ = DHCP_OPT_SUBNET;
	*o++ = DHCP_OPT_ROUTER;
	*o++ = DHCP_OPT_END;

	/* UDP header. */
	uint8_t *udp = frame + ETH_HDR_LEN + IPV4_HDR_LEN;
	unsigned udp_len = UDP_HDR_LEN + sizeof(struct dhcp_pkt);
	put_be16(udp + 0, DHCP_CLIENT_PORT);
	put_be16(udp + 2, DHCP_SERVER_PORT);
	put_be16(udp + 4, (uint16_t)udp_len);
	put_be16(udp + 6, 0);				/* checksum, fill below */
	uint16_t cs = udp_checksum(0u, 0xffffffffu, udp, udp_len);
	put_be16(udp + 6, cs);

	/* IPv4 header. */
	uint8_t *ip = frame + ETH_HDR_LEN;
	unsigned total_len = IPV4_HDR_LEN + udp_len;
	ip[0]  = 0x45;					/* IHL=5 v=4 */
	ip[1]  = 0x00;
	put_be16(ip + 2,  (uint16_t)total_len);
	put_be16(ip + 4,  0x0000);			/* id, frag */
	put_be16(ip + 6,  0x0000);
	ip[8]  = 64;					/* TTL */
	ip[9]  = IPPROTO_UDP;
	put_be16(ip + 10, 0);				/* checksum -- below */
	put_be32(ip + 12, 0x00000000u);			/* src 0.0.0.0 */
	put_be32(ip + 16, 0xffffffffu);			/* dst 255.255.255.255 */
	uint16_t ip_cs = ip_checksum(ip, IPV4_HDR_LEN);
	put_be16(ip + 10, ip_cs);

	return virtio_net_tx_bytes(frame, ETH_HDR_LEN + total_len);
}

/* Walk a DHCP reply's option blob looking for a single tag.
 * Returns a pointer to the option's value bytes (length in *len_out)
 * or NULL if not present.  Stops at OPT_END or a malformed length. */
static const uint8_t *dhcp_find_opt(const uint8_t *opts, unsigned cap,
                                    uint8_t tag, uint8_t *len_out)
{
	unsigned i = 0;
	while (i < cap) {
		uint8_t t = opts[i++];
		if (t == DHCP_OPT_END) return NULL;
		if (t == DHCP_OPT_PAD) continue;
		if (i >= cap) return NULL;
		uint8_t l = opts[i++];
		if (i + l > cap) return NULL;
		if (t == tag) { *len_out = l; return opts + i; }
		i += l;
	}
	return NULL;
}

/* Called from rx_dispatch when an Ethernet frame with UDP/68 lands.
 * Parses the DHCP message-type option and either advances the state
 * machine (OFFER -> snapshot offer; ACK -> mark bound) or drops the
 * packet silently if it doesn't match our pending xid. */
static void dhcp_handle_rx(const uint8_t *udp_payload, unsigned ulen)
{
	/* The DHCP fixed header is 240 bytes (ending at the magic
	 * cookie); options follow.  Anything smaller can't be a valid
	 * DHCP reply. */
	if (ulen < 240) return;
	const struct dhcp_pkt *d = (const struct dhcp_pkt *)udp_payload;
	if (d->op != DHCP_OP_BOOTREPLY) return;
	if (get_be32((const uint8_t *)&d->xid) != dhcp.xid) return;
	if (get_be32((const uint8_t *)&d->magic) != DHCP_MAGIC) return;

	unsigned opt_cap = (unsigned)(ulen - ((const uint8_t *)d->options - udp_payload));
	if (opt_cap > sizeof(d->options)) opt_cap = sizeof(d->options);

	uint8_t l;
	const uint8_t *mt = dhcp_find_opt(d->options, opt_cap,
	                                  DHCP_OPT_MSG_TYPE, &l);
	if (!mt || l != 1) return;

	if (mt[0] == DHCP_MSG_OFFER && dhcp.state == DHCP_WAIT_OFFER) {
		dhcp.offer_ip = get_be32((const uint8_t *)&d->yiaddr);
		const uint8_t *sm = dhcp_find_opt(d->options, opt_cap,
		                                  DHCP_OPT_SUBNET, &l);
		dhcp.offer_netmask = (sm && l == 4) ? get_be32(sm)
		                                    : FALLBACK_NETMASK;
		const uint8_t *rt = dhcp_find_opt(d->options, opt_cap,
		                                  DHCP_OPT_ROUTER, &l);
		dhcp.offer_gw = (rt && l == 4) ? get_be32(rt)
		                               : FALLBACK_GATEWAY;
		const uint8_t *sid = dhcp_find_opt(d->options, opt_cap,
		                                   DHCP_OPT_SERVER_ID, &l);
		dhcp.offer_server = (sid && l == 4) ? get_be32(sid)
		                                    : dhcp.offer_gw;
		dhcp.state = DHCP_WAIT_ACK;
	} else if (mt[0] == DHCP_MSG_ACK && dhcp.state == DHCP_WAIT_ACK) {
		dhcp.state = DHCP_BOUND;
	}
}

/* Hook called by rx_dispatch when an IPv4 frame arrives -- pulls out
 * the UDP destination port and, if it's our DHCP client port, hands
 * the UDP payload to dhcp_handle_rx.  Returns 1 if it was a DHCP
 * packet (consumed), 0 otherwise (caller continues normal IP path). */
static int dhcp_rx_filter(const uint8_t *frame, unsigned len)
{
	if (dhcp.state != DHCP_WAIT_OFFER
	 && dhcp.state != DHCP_WAIT_ACK) return 0;
	if (len < ETH_HDR_LEN + IPV4_HDR_LEN + UDP_HDR_LEN) return 0;
	const uint8_t *ip = frame + ETH_HDR_LEN;
	if ((ip[0] >> 4) != 4) return 0;
	unsigned ihl = (ip[0] & 0xf) * 4;
	if (ihl < IPV4_HDR_LEN) return 0;
	if (ip[9] != IPPROTO_UDP) return 0;
	if (len < ETH_HDR_LEN + ihl + UDP_HDR_LEN) return 0;
	const uint8_t *udp = ip + ihl;
	uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
	if (dport != DHCP_CLIENT_PORT) return 0;
	uint16_t ulen = (uint16_t)((udp[4] << 8) | udp[5]);
	if (ulen < UDP_HDR_LEN) return 1;
	dhcp_handle_rx(udp + UDP_HDR_LEN, ulen - UDP_HDR_LEN);
	return 1;
}

/* Wait up to `ms` milliseconds for `dhcp.state` to leave `expected`.
 * Returns 1 on state change, 0 on timeout.
 *
 * We're called from the boot context (kmain on the main thread)
 * with no other kthreads running yet, so kthread_sleep_on falls
 * through to cpu_idle == main_thread and returns immediately --
 * no actual sleep happens.  Use the generic-timer counter
 * (CNTPCT_EL0) and CNTFRQ_EL0 to busy-wait honestly while IRQs are
 * unmasked so virtio_net_irq can fire and deliver DHCP responses
 * into dhcp.state. */
static int dhcp_wait_state(int expected, unsigned ms)
{
	uint64_t freq, start, deadline;
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
	deadline = start + (freq / 1000) * (uint64_t)ms;
	for (;;) {
		if (dhcp.state != (int)expected) return 1;
		uint64_t now;
		__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
		if (now >= deadline) return 0;
		__asm__ volatile ("yield");
	}
}

/* Drive the DISCOVER/OFFER/REQUEST/ACK exchange synchronously.
 * Returns 1 on success (eth0_ip/netmask/gateway populated), 0 on
 * timeout (caller falls back to compile-time defaults). */
static int dhcp_run(void)
{
	/* Unmask DAIF.I so virtio_net_irq can fire during our busy-wait
	 * and deliver the OFFER/ACK packets into dhcp.state.  Save the
	 * old DAIF (kmain comes in with IRQs masked; the proper
	 * `daifclr` happens later from kmain's idle-loop epilogue) and
	 * restore on the way out so we don't mess with the rest of
	 * init's IRQ discipline. */
	uint64_t saved_daif;
	__asm__ volatile ("mrs %0, daif" : "=r"(saved_daif));
	__asm__ volatile ("msr daifclr, #2" ::: "memory");

	/* Use the low bits of our MAC + a boot-time tick as the
	 * transaction id.  Not cryptographic, just needs to be
	 * unique enough that we don't latch onto a stale OFFER. */
	dhcp.xid = ((uint32_t)g_eth0.mac[2] << 24)
	         | ((uint32_t)g_eth0.mac[3] << 16)
	         | ((uint32_t)g_eth0.mac[4] <<  8)
	         |  (uint32_t)g_eth0.mac[5];
	dhcp.wq = (struct wait_queue)WAIT_QUEUE_INIT;

	int success = 0;
	for (int attempt = 0; attempt < 3; attempt++) {
		dhcp.state = DHCP_WAIT_OFFER;
		if (dhcp_send(DHCP_MSG_DISCOVER, 0, 0) < 0) goto out;
		/* 2 s window per attempt.  slirp answers in <10 ms; on
		 * a real network we may need longer. */
		if (dhcp_wait_state(DHCP_WAIT_OFFER, 2000)) break;
	}
	if (dhcp.state != DHCP_WAIT_ACK) {
		kprintf("eth0: DHCP no OFFER -- using fallback\n");
		/* Field diagnostic: tx used > 0 means the device consumed
		 * our DISCOVER; rx used > last means replies landed but the
		 * IRQ never drained them (GIC delivery problem -- see the
		 * GICD_CTLR UEFI-handoff note in gic.c). */
		kprintf("eth0: tx used=%u avail=%u  rx used=%u drained=%u\n",
			g_eth0.tx.used->idx, g_eth0.tx.avail->idx,
			g_eth0.rx.used->idx, g_eth0.rx.last_used);
		goto out;
	}

	for (int attempt = 0; attempt < 3; attempt++) {
		if (dhcp_send(DHCP_MSG_REQUEST,
		              dhcp.offer_ip, dhcp.offer_server) < 0) goto out;
		if (dhcp_wait_state(DHCP_WAIT_ACK, 2000)) break;
	}
	if (dhcp.state != DHCP_BOUND) {
		kprintf("eth0: DHCP no ACK -- using fallback\n");
		goto out;
	}

	eth0_ip      = dhcp.offer_ip;
	eth0_netmask = dhcp.offer_netmask;
	eth0_gateway = dhcp.offer_gw;
	success = 1;
out:
	__asm__ volatile ("msr daif, %0" :: "r"(saved_daif) : "memory");
	return success;
}

static void wire_eth0_into_ip(void)
{
	struct stdata *sd =
	    stream_build_kernel(&eth0_streamtab, "eth0_drv", 0);
	if (!sd) {
		kprintf("eth0: stream_build_kernel failed\n");
		return;
	}
	eth0_sd = sd;

	eth0_nif.name     = "eth0";
	eth0_nif.ip       = eth0_ip;
	eth0_nif.netmask  = eth0_netmask;
	eth0_nif.mtu      = 1500;
	eth0_nif.streamtab = NULL;	/* using pre-built sd via attach */
	eth0_nif.tx       = NULL;
	netif_register(&eth0_nif);

	long muxid = ip_attach_stream(sd, &eth0_nif);
	if (muxid <= 0) {
		kprintf("eth0: ip_attach_stream failed rc=%ld\n", muxid);
		return;
	}
	kprintf("eth0: linked under IP muxid=%ld\n", muxid);
}

static int try_init_net(unsigned slot)
{
	uintptr_t base = PLAT_VIRTIO_MMIO_BASE
	               + (uintptr_t)slot * PLAT_VIRTIO_MMIO_STRIDE;
	uint32_t magic = *(volatile uint32_t *)(base + VMMIO_MAGIC);
	if (magic != VIRTIO_MAGIC) return -1;
	uint32_t ver = *(volatile uint32_t *)(base + VMMIO_VERSION);
	if (ver != 2) return -1;
	uint32_t did = *(volatile uint32_t *)(base + VMMIO_DEVICE_ID);
	if (did != VIRTIO_DEV_NET) return -1;

	g_eth0.mmio_base = base;
	g_eth0.slot      = slot;
	g_eth0.intid     = 32 + 16 + slot;

	mw32(VMMIO_STATUS, 0);
	mw32(VMMIO_STATUS, VS_ACKNOWLEDGE);
	mw32(VMMIO_STATUS, VS_ACKNOWLEDGE | VS_DRIVER);

	mw32(VMMIO_DEV_FEAT_SEL, 0);
	uint32_t feat_lo = mr32(VMMIO_DEV_FEAT);
	mw32(VMMIO_DRV_FEAT_SEL, 0);
	mw32(VMMIO_DRV_FEAT, feat_lo & (VNET_F_MAC | VNET_F_STATUS));
	mw32(VMMIO_DRV_FEAT_SEL, 1);
	mw32(VMMIO_DRV_FEAT, 0);
	mw32(VMMIO_STATUS, VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK);
	if (!(mr32(VMMIO_STATUS) & VS_FEATURES_OK)) {
		kprintf("virtio-net: FEATURES_OK rejected\n");
		mw32(VMMIO_STATUS, VS_FAILED);
		return -1;
	}

	for (int i = 0; i < 6; i++)
		g_eth0.mac[i] = *(volatile uint8_t *)(base + VMMIO_CONFIG + i);

	if (vq_init(&g_eth0.rx, RXQ) < 0) return -1;
	if (vq_init(&g_eth0.tx, TXQ) < 0) return -1;
	if (rx_prepare() < 0) return -1;
	if (tx_prepare() < 0) return -1;

	mw32(VMMIO_STATUS, VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK
	                 | VS_DRIVER_OK);

	gic_enable_irq(g_eth0.intid);

	kprintf("virtio-net: eth0 slot %u intid %u "
		"mac %02x:%02x:%02x:%02x:%02x:%02x\n",
		slot, g_eth0.intid,
		g_eth0.mac[0], g_eth0.mac[1], g_eth0.mac[2],
		g_eth0.mac[3], g_eth0.mac[4], g_eth0.mac[5]);

	/* Shared ARP cache identity: valid before RX starts so
	 * arp_input can learn from the first broadcast we see. */
	eth0_nif.name     = "eth0";	/* re-set at netif_register; ARP
					 * entries need it before that */
	eth0_arpif.nif    = &eth0_nif;
	eth0_arpif.mac    = g_eth0.mac;
	eth0_arpif.tx     = eth0_arp_tx;
	eth0_arpif.cookie = 0;
	arp_ifattach(&eth0_arpif);

	g_eth0.ready = 1;

	/* DHCP first, then plumb the IP layer.  If DHCP times out we
	 * fall back to the historic well-known QEMU slirp values so
	 * the kernel still gets onto a usable address.  IRQs are
	 * already on (gic_enable_irq above) so rx_dispatch can deliver
	 * the OFFER/ACK frames to dhcp_handle_rx via the dhcp_rx_filter
	 * hook. */
	if (!dhcp_run()) {
		eth0_ip      = FALLBACK_IP;
		eth0_netmask = FALLBACK_NETMASK;
		eth0_gateway = FALLBACK_GATEWAY;
	} else {
		/* Count 1 bits in netmask -- can't use __builtin_popcount
		 * because gcc lowers it through libgcc's __popcountdi2 which
		 * pulls in SIMD instructions, and we build with
		 * -mgeneral-regs-only. */
		unsigned nm_bits = 0;
		uint32_t m = eth0_netmask;
		while (m) { nm_bits += m & 1u; m >>= 1; }
		kprintf("eth0: DHCP bound %u.%u.%u.%u/%u gw %u.%u.%u.%u\n",
			(eth0_ip >> 24) & 0xff, (eth0_ip >> 16) & 0xff,
			(eth0_ip >>  8) & 0xff,  eth0_ip        & 0xff,
			nm_bits,
			(eth0_gateway >> 24) & 0xff, (eth0_gateway >> 16) & 0xff,
			(eth0_gateway >>  8) & 0xff,  eth0_gateway        & 0xff);
	}
	wire_eth0_into_ip();
	return 0;
}

void virtio_net_init(void)
{
	/* QEMU virt places devices starting from the HIGHEST slot, so we
	 * scan top-down to find the net device with the least latency. */
	for (int slot = (int)PLAT_VIRTIO_MMIO_MAX - 1; slot >= 0; slot--) {
		if (try_init_net((unsigned)slot) == 0)
			return;
	}
	kprintf("virtio-net: no device found in MMIO slots 0..%u\n",
		(unsigned)PLAT_VIRTIO_MMIO_MAX);
}

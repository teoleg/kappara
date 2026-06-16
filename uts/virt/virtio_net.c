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

struct arp_pkt {
	uint16_t hw_type;	/* 1 = Ethernet */
	uint16_t proto_type;	/* 0x0800 IPv4 */
	uint8_t  hw_len;	/* 6 */
	uint8_t  proto_len;	/* 4 */
	uint16_t opcode;	/* 1 = request, 2 = reply */
	uint8_t  sender_mac[6];
	uint8_t  sender_ip[4];
	uint8_t  target_mac[6];
	uint8_t  target_ip[4];
} __attribute__((packed));

#define ARP_REQUEST	1
#define ARP_REPLY	2

/* eth0's IPv4 config matches QEMU's default user-mode (slirp) NAT
 * subnet: 10.0.2.0/24 with host=10.0.2.2 (gateway), dns=10.0.2.3,
 * DHCP-assigned guest = 10.0.2.15.  We skip DHCP for now and just
 * use the well-known guest address. */
#define ETH0_IP		0x0a00020fu	/* 10.0.2.15 */
#define ETH0_NETMASK	0xffffff00u	/* /24 */
#define ETH0_GATEWAY	0x0a000202u	/* 10.0.2.2 */

/* ARP cache: just the gateway entry for now.  Resolved lazily via
 * the first ARP request we send out. */
static uint8_t arp_gw_mac[6];
static int     arp_gw_have;

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
static uint16_t get_be16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

/* Build an ARP frame in `frame` and TX it.  out_op = ARP_REQUEST or
 * ARP_REPLY; out_dst_mac may be broadcast for requests, the requester
 * for replies; sender/target_ip are host-byte-order. */
static void arp_send(int out_op, const uint8_t out_dst_mac[6],
                     uint32_t sender_ip, uint32_t target_ip,
                     const uint8_t target_mac[6])
{
	uint8_t frame[ETH_HDR_LEN + sizeof(struct arp_pkt)];
	struct ether_hdr *eh = (struct ether_hdr *)frame;
	kmemcpy(eh->dst, out_dst_mac, 6);
	kmemcpy(eh->src, g_eth0.mac, 6);
	eh->ethertype = (uint16_t)((ETHERTYPE_ARP & 0xff) << 8
	                         | ((ETHERTYPE_ARP >> 8) & 0xff));

	struct arp_pkt *a = (struct arp_pkt *)(frame + ETH_HDR_LEN);
	put_be16((uint8_t *)&a->hw_type,    1);
	put_be16((uint8_t *)&a->proto_type, ETHERTYPE_IPV4);
	a->hw_len    = 6;
	a->proto_len = 4;
	put_be16((uint8_t *)&a->opcode, (uint16_t)out_op);
	kmemcpy(a->sender_mac, g_eth0.mac, 6);
	a->sender_ip[0] = (sender_ip >> 24) & 0xff;
	a->sender_ip[1] = (sender_ip >> 16) & 0xff;
	a->sender_ip[2] = (sender_ip >>  8) & 0xff;
	a->sender_ip[3] = (sender_ip      ) & 0xff;
	if (target_mac)
		kmemcpy(a->target_mac, target_mac, 6);
	else
		kmemset(a->target_mac, 0, 6);
	a->target_ip[0] = (target_ip >> 24) & 0xff;
	a->target_ip[1] = (target_ip >> 16) & 0xff;
	a->target_ip[2] = (target_ip >>  8) & 0xff;
	a->target_ip[3] = (target_ip      ) & 0xff;

	virtio_net_tx_bytes(frame, sizeof(frame));
}

static void arp_send_request(uint32_t target_ip)
{
	static const uint8_t bcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
	arp_send(ARP_REQUEST, bcast, ETH0_IP, target_ip, 0);
}

/* Forward declaration: queue_t to attach to RX delivery path. */
static struct stdata *eth0_sd;

static void rx_dispatch(const uint8_t *frame, unsigned len)
{
	if (len < ETH_HDR_LEN) return;
	const struct ether_hdr *eh = (const struct ether_hdr *)frame;
	uint16_t et = (uint16_t)(((eh->ethertype & 0xff) << 8)
	                         | ((eh->ethertype >> 8) & 0xff));

	if (et == ETHERTYPE_ARP && len >= ETH_HDR_LEN + sizeof(struct arp_pkt)) {
		const struct arp_pkt *a =
		    (const struct arp_pkt *)(frame + ETH_HDR_LEN);
		uint16_t op = get_be16((const uint8_t *)&a->opcode);
		uint32_t sip = ((uint32_t)a->sender_ip[0] << 24)
		             | ((uint32_t)a->sender_ip[1] << 16)
		             | ((uint32_t)a->sender_ip[2] <<  8)
		             | ((uint32_t)a->sender_ip[3]      );
		uint32_t tip = ((uint32_t)a->target_ip[0] << 24)
		             | ((uint32_t)a->target_ip[1] << 16)
		             | ((uint32_t)a->target_ip[2] <<  8)
		             | ((uint32_t)a->target_ip[3]      );
		if (op == ARP_REQUEST && tip == ETH0_IP) {
			arp_send(ARP_REPLY, a->sender_mac, ETH0_IP,
			         sip, a->sender_mac);
		} else if (op == ARP_REPLY && sip == ETH0_GATEWAY) {
			kmemcpy(arp_gw_mac, a->sender_mac, 6);
			arp_gw_have = 1;
			kprintf("eth0: gw %02x:%02x:%02x:%02x:%02x:%02x\n",
			        arp_gw_mac[0], arp_gw_mac[1], arp_gw_mac[2],
			        arp_gw_mac[3], arp_gw_mac[4], arp_gw_mac[5]);
		}
		return;
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

	if (!arp_gw_have) {
		/* Trigger ARP and drop this packet -- the next packet
		 * after the reply lands will succeed. */
		arp_send_request(ETH0_GATEWAY);
		freemsg(mp);
		return 0;
	}

	uint8_t buf[ETH_HDR_LEN + 1500];
	struct ether_hdr *eh = (struct ether_hdr *)buf;
	kmemcpy(eh->dst, arp_gw_mac, 6);
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
	eth0_nif.ip       = ETH0_IP;
	eth0_nif.netmask  = ETH0_NETMASK;
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

	g_eth0.ready = 1;
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

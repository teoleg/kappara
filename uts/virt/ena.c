/*
 * uts/virt/ena.c -- AWS ENA network driver (SVR4 STREAMS)
 *
 * Amazon Elastic Network Adapter.  PCI vendor 0x1d0f, device 0xec20/0xec21.
 * Probed via pci_devs[], BAR0 mapped with mmu_map_device_1gb, admin queue
 * polled (interrupts masked).
 *
 * Stages implemented:
 *   E1  Admin hardening: GET_FEATURE(MAX_QUEUES), GET_FEATURE(LLQ),
 *       SET_FEATURE(HOST_ATTR_CONFIG).  Queue depth clamped to device max;
 *       placement_policy set from LLQ negotiation result.
 *   E2  Ethernet + ARP layer: ether_hdr, arp_pkt, ARP cache (gateway MAC),
 *       arp_send / arp_send_request / ena_arp_process.
 *   E3  TX ring: ena_eth_tx posts ena_io_tx_desc with dc cvac + dsb sy
 *       cache flush before the SQ doorbell; ena_tx_drain reclaims bounce
 *       buffers by polling the TX CQ (dc ivac before reading phase bit).
 *   E4  RX kthread: ena_rx_poll_one drains one RX CQ entry per call,
 *       dc ivac on both the CQ entry and the data buffer before reading;
 *       ena_rx_repost refills the slot.  ena_rx_thread yields when idle.
 *   E5  DHCP client: raw-Ethernet DISCOVER→OFFER→REQUEST→ACK state machine
 *       spin-polling the ENA RX CQ directly (no IRQ, no kthread yet).
 *       10-second timeout; no fallback on Graviton (no slirp).
 *   E6  STREAMS personality: ena_streamtab (ena_wq_putp / ena_rq_putp /
 *       ena_qopen).  Write side prepends Ethernet header, calls ena_eth_tx.
 *       Read side is a pass-through putnext.
 *   E7  IP wire-up: wire_ena_into_ip builds a kernel stream from
 *       ena_streamtab, calls ip_attach_stream, registers the netif, then
 *       starts the ena_rx kthread.
 *
 * Cache maintenance (belt-and-braces; Graviton PCIe DMA is coherent):
 *   TX: cache_clean_range on the buffer and SQ descriptor before doorbell.
 *   RX CQ: cache_inval_range on the CQ entry before reading the phase bit.
 *   RX data: cache_inval_range on the buffer before reading received bytes.
 *   RX repost: cache_clean_range on the updated RX SQ descriptor.
 *
 * I/O descriptors carry a per-pass phase bit (TX len_ctrl[24], RX
 * ctrl[0]) that must match the SQ's current phase or the device
 * ignores the entry; phase starts at 1 and flips on each ring wrap.
 *
 * Register offsets and bit definitions cross-checked against:
 *   torvalds/linux drivers/net/ethernet/amazon/ena/ena_regs_defs.h
 *   torvalds/linux drivers/net/ethernet/amazon/ena/ena_admin_defs.h
 *   torvalds/linux drivers/net/ethernet/amazon/ena/ena_eth_io_defs.h
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/arch/ena.h"
#include "kappara/arch/mmu.h"
#include "kappara/net/arp.h"
#include "kappara/arch/pcie.h"
#include "kappara/core/kmem.h"
#include "kappara/core/pmm.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"
#include "kappara/io/stream_head.h"
#include "kappara/io/streams.h"
#include "kappara/net/ip.h"
#include "kappara/net/netif.h"
#include "kappara/proc/sched.h"

/* ---- PCI IDs ---- */
#define ENA_PCI_VID		0x1d0f
#define ENA_PCI_DID_EC20	0xec20
#define ENA_PCI_DID_EC21	0xec21
#define ENA_PCI_CLASS_NET	0x0200

/* ---- BAR0 register offsets (ena_regs_defs.h) ---- */
#define ENA_REG_VERSION			0x00
#define ENA_REG_CONTROLLER_VERSION	0x04
#define ENA_REG_CAPS			0x08
#define ENA_REG_CAPS_EXT		0x0C
#define ENA_REG_AQ_BASE_LO		0x10
#define ENA_REG_AQ_BASE_HI		0x14
#define ENA_REG_AQ_CAPS			0x18
#define ENA_REG_ACQ_BASE_LO		0x20
#define ENA_REG_ACQ_BASE_HI		0x24
#define ENA_REG_ACQ_CAPS		0x28
#define ENA_REG_AQ_DB			0x2C
#define ENA_REG_ACQ_TAIL		0x30
#define ENA_REG_AENQ_CAPS		0x34
#define ENA_REG_AENQ_BASE_LO		0x38
#define ENA_REG_AENQ_BASE_HI		0x3C
#define ENA_REG_AENQ_HEAD_DB		0x40
#define ENA_REG_AENQ_TAIL		0x44
#define ENA_REG_INTR_MASK		0x4C
#define ENA_REG_DEV_CTL			0x54
#define ENA_REG_DEV_STS			0x58
#define ENA_REG_MMIO_REG_READ		0x5C
#define ENA_REG_MMIO_RESP_LO		0x60
#define ENA_REG_MMIO_RESP_HI		0x64

#define ENA_CAPS_RESET_TIMEOUT_SHIFT	1
#define ENA_CAPS_RESET_TIMEOUT_MASK	0x3E

#define ENA_DEV_CTL_RESET		(1u << 0)
#define ENA_DEV_CTL_AQ_RESTART		(1u << 1)
#define ENA_DEV_CTL_QUIESCENT		(1u << 2)
#define ENA_DEV_CTL_IO_RESUME		(1u << 3)
#define ENA_DEV_CTL_RESET_REASON_SHIFT	28

#define ENA_DEV_STS_READY		(1u << 0)
#define ENA_DEV_STS_AQ_RESTART_PROG	(1u << 1)
#define ENA_DEV_STS_AQ_RESTART_DONE	(1u << 2)
#define ENA_DEV_STS_RESET_IN_PROGRESS	(1u << 3)
#define ENA_DEV_STS_RESET_FINISHED	(1u << 4)
#define ENA_DEV_STS_FATAL_ERROR		(1u << 5)

#define ENA_QCAPS(depth, entry_size) \
	(((uint32_t)(entry_size) << 16) | (uint32_t)(depth))

/* ---- Admin queue structures ---- */

struct ena_admin_aq_common_desc {
	uint16_t command_id;
	uint8_t  opcode;
	uint8_t  flags;		/* bit[0]=phase, bit[1]=ctrl_data */
} __attribute__((packed));

struct ena_admin_acq_common_desc {
	uint16_t command;	/* bits[11:0]=command_id echoed */
	uint8_t  status;
	uint8_t  flags;		/* bit[0]=phase tag */
	uint16_t extended_status;
	uint16_t sq_head_indx;
} __attribute__((packed));

struct ena_admin_aq_entry {
	struct ena_admin_aq_common_desc common;
	uint8_t                         payload[60];
} __attribute__((packed));

struct ena_admin_acq_entry {
	struct ena_admin_acq_common_desc common;
	uint8_t                          payload[56];
} __attribute__((packed));

struct ena_aenq_entry {
	uint16_t group;
	uint16_t syndrome;
	uint8_t  flags;
	uint8_t  reserved[3];
	uint64_t timestamp_us;
	uint8_t  payload[48];
} __attribute__((packed));

/* opcodes */
#define ENA_ADMIN_OP_CREATE_SQ		1
#define ENA_ADMIN_OP_DESTROY_SQ		2
#define ENA_ADMIN_OP_CREATE_CQ		3
#define ENA_ADMIN_OP_DESTROY_CQ		4
#define ENA_ADMIN_OP_GET_FEATURE	8
#define ENA_ADMIN_OP_SET_FEATURE	9

/* Feature IDs */
#define ENA_ADMIN_FEAT_DEVICE_ATTRIBUTES	1
#define ENA_ADMIN_FEAT_MAX_QUEUES_NUM		2
#define ENA_ADMIN_FEAT_LLQ			4
#define ENA_ADMIN_FEAT_HOST_ATTR_CONFIG		16

struct ena_admin_feat_common {
	uint8_t  flags;
	uint8_t  feature_id;
	uint16_t feature_version;
} __attribute__((packed));

struct ena_admin_get_feat_dev_attr {
	uint32_t impl_id;
	uint32_t device_version;
	uint32_t supported_features;
	uint32_t reserved3;
	uint32_t phys_addr_width;
	uint32_t virt_addr_width;
	uint8_t  mac_addr[6];
	uint8_t  reserved7[2];
	uint32_t max_mtu;
} __attribute__((packed));

struct ena_mmio_read_resp {
	uint16_t req_id;
	uint16_t reg_off;
	uint32_t reg_val;
} __attribute__((packed));

/* ---- I/O queue descriptors (ena_eth_io_defs.h layouts) ---- */

/* TX submission descriptor: 16 bytes.
 * len_ctrl:  [15:0] length  [21:16] req_id_hi  [23] meta_desc
 *            [24] phase  [26] first  [27] last  [28] comp_req
 * meta_ctrl: [26:22] req_id_lo; all other bits are offload knobs
 *            (l3/l4 proto, csum enables) -- zero for raw frames. */
struct ena_io_tx_desc {
	uint32_t len_ctrl;
	uint32_t meta_ctrl;
	uint32_t buff_addr_lo;
	uint32_t buff_addr_hi_hdr;	/* [15:0] addr_hi  [31:24] header_length */
} __attribute__((packed));

#define ENA_TX_DESC_PHASE	(1u << 24)
#define ENA_TX_DESC_FIRST	(1u << 26)
#define ENA_TX_DESC_LAST	(1u << 27)
#define ENA_TX_DESC_COMP_REQ	(1u << 28)

/* RX submission descriptor: 16 bytes.
 * ctrl: [0] phase  [2] first  [3] last  [4] comp_req */
struct ena_io_rx_desc {
	uint16_t length;
	uint8_t  reserved;
	uint8_t  ctrl;
	uint16_t req_id;
	uint16_t reserved2;
	uint32_t buff_addr_lo;
	uint16_t buff_addr_hi;
	uint16_t reserved3;
} __attribute__((packed));

#define ENA_RX_DESC_PHASE	0x01u
#define ENA_RX_DESC_FIRST	0x04u
#define ENA_RX_DESC_LAST	0x08u
#define ENA_RX_DESC_COMP_REQ	0x10u

/* TX completion: 2 words (8 bytes). */
struct ena_io_tx_cdesc {
	uint16_t req_id;
	uint8_t  status;
	uint8_t  flags;		/* bit[0]=phase tag */
	uint16_t sub_qid;
	uint16_t sq_head_idx;	/* ignored by us */
} __attribute__((packed));

/* RX completion (ena_eth_io_rx_cdesc_base): 4 words (16 bytes).
 * status: [24] phase  [26] first  [27] last; low bits are l3/l4
 * proto + csum results, ignored for our raw-frame consumption. */
struct ena_io_rx_cdesc {
	uint32_t status;
	uint16_t length;	/* actual bytes received */
	uint16_t req_id;
	uint32_t hash;
	uint16_t sub_qid;
	uint8_t  offset;
	uint8_t  reserved;
} __attribute__((packed));

#define ENA_RX_CDESC_PHASE	(1u << 24)

/* ---- Ethernet + ARP ---- */

struct ena_ether_hdr {
	uint8_t  dst[6];
	uint8_t  src[6];
	uint16_t ethertype;	/* big-endian */
} __attribute__((packed));

#define ENA_ETH_HDR_LEN		14
#define ENA_ETHERTYPE_IPV4	0x0800
#define ENA_ETHERTYPE_ARP	0x0806

/* ---- DHCP structures ---- */

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
	uint32_t xid;
	uint16_t secs;
	uint16_t flags;
	uint32_t ciaddr;
	uint32_t yiaddr;
	uint32_t siaddr;
	uint32_t giaddr;
	uint8_t  chaddr[16];
	uint8_t  sname[64];
	uint8_t  file[128];
	uint32_t magic;
	uint8_t  options[312];
} __attribute__((packed));

enum {
	DHCP_INIT,
	DHCP_WAIT_OFFER,
	DHCP_WAIT_ACK,
	DHCP_BOUND,
	DHCP_FAILED,
};

/* ---- Driver state ---- */

#define ENA_AQ_DEPTH		32
#define ENA_IOQ_DEPTH		64
#define ENA_RX_BUF_BYTES	2048

struct ena_io_q {
	void    *sq_pa;
	void    *cq_pa;
	uint32_t depth;
	uint16_t sq_tail;
	uint16_t cq_head;
	uint8_t  cq_phase;
	uint8_t  sq_phase;
	uint32_t sq_db_off;
	uint32_t cq_db_off;
};

struct ena_dev {
	volatile uint8_t *regs;

	struct ena_mmio_read_resp  *mmio_resp;
	uint16_t                    mmio_seq;

	struct ena_admin_aq_entry  *aq;
	struct ena_admin_acq_entry *acq;
	struct ena_aenq_entry      *aenq;
	uint16_t aq_tail;
	uint16_t acq_head;
	uint8_t  acq_phase;
	uint8_t  aq_phase;
	uint16_t aenq_head;
	uint8_t  aenq_phase;
	uint16_t next_cmd_id;

	uint8_t  mac[6];
	uint32_t max_mtu;
	uint32_t max_sq_depth;	/* from GET_FEATURE(MAX_QUEUES) */
	int      use_llq;	/* placement_policy: 1=host_mem 3=LLQ */

	struct ena_io_q tx;
	struct ena_io_q rx;

	void    *rx_bufs[ENA_IOQ_DEPTH];
	void    *tx_bufs[ENA_IOQ_DEPTH];	/* bounce buffers; freed on TX CQ */

	int      io_ready;		/* TX+RX rings set up; checked by ena_eth_tx */

	/* Ethernet/IP addressing */
	uint32_t ip;
	uint32_t netmask;
	uint32_t gateway;

	/* Shared ARP cache identity (uts/os/net/arp.c, /proc/arp). */
	struct arpif aif;

	/* DHCP state machine */
	int      dhcp_state;
	uint32_t dhcp_xid;
	uint32_t dhcp_offer_ip;
	uint32_t dhcp_offer_netmask;
	uint32_t dhcp_offer_gw;
	uint32_t dhcp_offer_server;

	/* Field diagnostic: first frame received while DHCP was still
	 * unbound that the filter did NOT consume.  Hexdumped on DHCP
	 * failure so an AWS boot log shows what actually arrived. */
	uint8_t  dbg_rx[48];
	unsigned dbg_rx_len;

	/* STREAMS */
	struct stdata *ena_sd;	/* set by wire_ena_into_ip */
	struct netif   nif;
};

static struct ena_dev g_ena;
static int            g_ena_init_ok;

/* ---- MMIO accessors ---- */

static uint32_t r32(volatile uint8_t *base, unsigned off)
{
	return *(volatile uint32_t *)(base + off);
}
static void w32(volatile uint8_t *base, unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

/* Cache maintenance over a full byte range, not just the first line.
 * A single `dc cvac, x` covers one 64-byte line; a 590-byte DHCP
 * frame needs the whole span cleaned or the device reads stale
 * tails.  (PCIe DMA on Graviton is cache-coherent so these are
 * belt-and-braces, but if they're here they must be correct.) */
static void cache_clean_range(const void *p, unsigned len)
{
	uintptr_t a   = (uintptr_t)p & ~63UL;
	uintptr_t end = (uintptr_t)p + len;
	for (; a < end; a += 64)
		__asm__ volatile ("dc cvac, %0" :: "r"(a) : "memory");
	__asm__ volatile ("dsb sy" ::: "memory");
}
static void cache_inval_range(const void *p, unsigned len)
{
	uintptr_t a   = (uintptr_t)p & ~63UL;
	uintptr_t end = (uintptr_t)p + len;
	for (; a < end; a += 64)
		__asm__ volatile ("dc ivac, %0" :: "r"(a) : "memory");
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* ---- MMIO read-less mechanism ---- */
#define ENA_RESET_CAPS_MAX_UNITS	40

static void ena_mmio_read_init(struct ena_dev *d)
{
	d->mmio_resp = pmm_alloc();
	if (!d->mmio_resp) {
		kprintf("ena: PMM exhausted for MMIO read buffer\n");
		return;
	}
	kmemset(d->mmio_resp, 0, 4096);
	d->mmio_seq = 1;

	uint64_t pa = (uint64_t)(uintptr_t)d->mmio_resp;
	w32(d->regs, ENA_REG_MMIO_RESP_LO, (uint32_t)pa);
	w32(d->regs, ENA_REG_MMIO_RESP_HI, (uint32_t)(pa >> 32));
}

static uint32_t ena_r32(struct ena_dev *d, uint16_t off)
{
	if (!d->mmio_resp) return r32(d->regs, off);

	uint16_t seq = d->mmio_seq++;
	if (d->mmio_seq == 0) d->mmio_seq = 1;

	__asm__ volatile ("dc ivac, %0\n\t" "dsb sy\n\t"
			  :: "r"(d->mmio_resp) : "memory");

	w32(d->regs, ENA_REG_MMIO_REG_READ,
	    (uint32_t)seq | ((uint32_t)off << 16));

	uint64_t freq, start, deadline;
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
	deadline = start + (freq / 1000ULL);
	for (;;) {
		__asm__ volatile ("dc ivac, %0\n\t" "dsb sy\n\t"
				  :: "r"(d->mmio_resp) : "memory");
		if (d->mmio_resp->req_id == seq)
			return d->mmio_resp->reg_val;
		uint64_t now;
		__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
		if (now >= deadline) break;
	}
	return r32(d->regs, off);
}

/* ---- Reset ---- */

static int wait_dev_sts_ms(struct ena_dev *d, uint32_t mask,
			   uint32_t expected, unsigned ms)
{
	uint64_t freq, start, deadline;
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
	deadline = start + (freq / 1000ULL) * (uint64_t)ms;
	for (;;) {
		uint32_t s = ena_r32(d, ENA_REG_DEV_STS);
		if ((s & mask) == expected) return (int)s;
		if (s & ENA_DEV_STS_FATAL_ERROR) {
			kprintf("ena: FATAL_ERROR in DEV_STS (0x%x)\n", s);
			return -1;
		}
		uint64_t now;
		__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
		if (now >= deadline) return -2;
	}
}

static int ena_reset(struct ena_dev *d)
{
	uint32_t caps    = ena_r32(d, ENA_REG_CAPS);
	uint32_t t_units = (caps & ENA_CAPS_RESET_TIMEOUT_MASK) >>
			   ENA_CAPS_RESET_TIMEOUT_SHIFT;
	if (t_units == 0) t_units = 8;
	if (t_units > ENA_RESET_CAPS_MAX_UNITS) t_units = ENA_RESET_CAPS_MAX_UNITS;
	unsigned timeout_ms = t_units * 100;
	kprintf("ena: reset  caps=0x%x  timeout=%ux100ms\n", caps, t_units);

	uint32_t s = ena_r32(d, ENA_REG_DEV_STS);

	if (s & ENA_DEV_STS_RESET_IN_PROGRESS) {
		kprintf("ena: prior reset in progress (STS=0x%x), clearing CTL\n", s);
		w32(d->regs, ENA_REG_DEV_CTL, 0);
		int rc = wait_dev_sts_ms(d, ENA_DEV_STS_RESET_IN_PROGRESS,
					 0, timeout_ms);
		if (rc < 0)
			kprintf("ena: prior reset stuck (STS=0x%x), continuing\n",
				ena_r32(d, ENA_REG_DEV_STS));
		else
			kprintf("ena: prior reset cleared\n");
		s = ena_r32(d, ENA_REG_DEV_STS);
	}

	if (!(s & ENA_DEV_STS_READY)) {
		int rc = wait_dev_sts_ms(d, ENA_DEV_STS_READY, ENA_DEV_STS_READY,
					 timeout_ms);
		if (rc < 0) {
			kprintf("ena: not READY before reset (STS=0x%x)\n",
				ena_r32(d, ENA_REG_DEV_STS));
			return -1;
		}
	}

	w32(d->regs, ENA_REG_DEV_CTL, ENA_DEV_CTL_RESET);
	if (d->mmio_resp) {
		uint64_t pa = (uint64_t)(uintptr_t)d->mmio_resp;
		w32(d->regs, ENA_REG_MMIO_RESP_LO, (uint32_t)pa);
		w32(d->regs, ENA_REG_MMIO_RESP_HI, (uint32_t)(pa >> 32));
	}

	{
		uint64_t freq, start, deadline;
		__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
		__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
		deadline = start + (freq / 1000ULL) * 1000ULL;
		for (;;) {
			s = ena_r32(d, ENA_REG_DEV_STS);
			if (s & ENA_DEV_STS_RESET_IN_PROGRESS) break;
			if (s & ENA_DEV_STS_FATAL_ERROR) {
				kprintf("ena: fatal during reset (STS=0x%x)\n", s);
				return -1;
			}
			uint64_t now;
			__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
			if (now >= deadline) {
				kprintf("ena: RESET_IN_PROGRESS never set (STS=0x%x)\n", s);
				return -1;
			}
		}
	}

	w32(d->regs, ENA_REG_DEV_CTL, 0);

	int rc = wait_dev_sts_ms(d, ENA_DEV_STS_RESET_IN_PROGRESS,
				 0, timeout_ms);
	if (rc < 0) {
		kprintf("ena: reset timed out (STS=0x%x after %ums)\n",
			ena_r32(d, ENA_REG_DEV_STS), timeout_ms);
		return -1;
	}

	rc = wait_dev_sts_ms(d, ENA_DEV_STS_READY, ENA_DEV_STS_READY, timeout_ms);
	if (rc < 0) {
		kprintf("ena: timed out waiting for READY (STS=0x%x)\n",
			ena_r32(d, ENA_REG_DEV_STS));
		return -1;
	}
	kprintf("ena: reset complete (STS=0x%x)\n", ena_r32(d, ENA_REG_DEV_STS));
	return 0;
}

/* ---- Admin queue setup ---- */

static int ena_admin_init(struct ena_dev *d)
{
	d->aq   = pmm_alloc();
	d->acq  = pmm_alloc();
	d->aenq = pmm_alloc();
	if (!d->aq || !d->acq || !d->aenq) {
		kprintf("ena: PMM exhausted during admin init\n");
		return -1;
	}
	kmemset(d->aq,   0, 4096);
	kmemset(d->acq,  0, 4096);
	kmemset(d->aenq, 0, 4096);
	d->aq_tail     = 0;
	d->acq_head    = 0;
	d->acq_phase   = 1;
	d->aq_phase    = 0;
	d->aenq_head   = 0;
	d->aenq_phase  = 1;
	d->next_cmd_id = 1;

	uint64_t aq_pa   = (uint64_t)(uintptr_t)d->aq;
	uint64_t acq_pa  = (uint64_t)(uintptr_t)d->acq;
	uint64_t aenq_pa = (uint64_t)(uintptr_t)d->aenq;

	w32(d->regs, ENA_REG_AQ_BASE_LO, (uint32_t)aq_pa);
	w32(d->regs, ENA_REG_AQ_BASE_HI, (uint32_t)(aq_pa >> 32));
	w32(d->regs, ENA_REG_AQ_CAPS,
	    ENA_QCAPS(ENA_AQ_DEPTH, sizeof(struct ena_admin_aq_entry)));

	w32(d->regs, ENA_REG_ACQ_BASE_LO, (uint32_t)acq_pa);
	w32(d->regs, ENA_REG_ACQ_BASE_HI, (uint32_t)(acq_pa >> 32));
	w32(d->regs, ENA_REG_ACQ_CAPS,
	    ENA_QCAPS(ENA_AQ_DEPTH, sizeof(struct ena_admin_acq_entry)));

	w32(d->regs, ENA_REG_AENQ_BASE_LO, (uint32_t)aenq_pa);
	w32(d->regs, ENA_REG_AENQ_BASE_HI, (uint32_t)(aenq_pa >> 32));
	w32(d->regs, ENA_REG_AENQ_CAPS,
	    ENA_QCAPS(ENA_AQ_DEPTH, sizeof(struct ena_aenq_entry)));
	w32(d->regs, ENA_REG_AENQ_TAIL, 0);

	w32(d->regs, ENA_REG_INTR_MASK, 0xFFFFFFFFu);
	return 0;
}

/* Submit one admin command; poll ACQ for the matching CQE.
 * Returns 0 on success, device status (>0) on device error, -1 on timeout. */
static int ena_admin_submit(struct ena_dev *d,
			    struct ena_admin_aq_entry *cmd,
			    void *resp)
{
	uint16_t cmd_id = d->next_cmd_id++;

	struct ena_admin_aq_entry *slot = &d->aq[d->aq_tail];
	*slot = *cmd;
	slot->common.command_id = cmd_id;
	slot->common.flags = (uint8_t)((slot->common.flags & ~1u) | d->aq_phase);

	__asm__ volatile ("dsb sy" ::: "memory");

	d->aq_tail++;
	if (d->aq_tail >= ENA_AQ_DEPTH) {
		d->aq_tail = 0;
		d->aq_phase ^= 1;
	}
	w32(d->regs, ENA_REG_AQ_DB, d->aq_tail);

	uint64_t _freq, _start, _deadline;
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(_freq));
	__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(_start));
	_deadline = _start + _freq * 5ULL;
	for (;;) {
		struct ena_admin_acq_entry *e = &d->acq[d->acq_head];
		__asm__ volatile ("dc ivac, %0\n\t" "dsb sy\n\t"
				  :: "r"(e) : "memory");
		uint8_t  phase = e->common.flags & 1u;
		uint16_t cid   = e->common.command & 0x0FFFu;
		if (phase == d->acq_phase && cid == cmd_id) {
			uint8_t status = e->common.status;
			if (resp) kmemcpy(resp, e->payload, 56);
			d->acq_head++;
			if (d->acq_head >= ENA_AQ_DEPTH) {
				d->acq_head = 0;
				d->acq_phase ^= 1;
			}
			w32(d->regs, ENA_REG_ACQ_TAIL, d->acq_head);
			return (int)status;
		}
		uint64_t _now;
		__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(_now));
		if (_now >= _deadline) {
			kprintf("ena: admin cmd %u timeout (opcode=%u)\n",
				cmd_id, cmd->common.opcode);
			return -1;
		}
	}
}

/* Helper: build a GET_FEATURE or SET_FEATURE command skeleton. */
static void ena_feat_cmd(struct ena_admin_aq_entry *cmd,
			 uint8_t opcode, uint8_t feat_id)
{
	kmemset(cmd, 0, sizeof(*cmd));
	cmd->common.opcode = opcode;
	struct ena_admin_feat_common *fc =
		(struct ena_admin_feat_common *)(cmd->payload + 12);
	fc->feature_id = feat_id;
}

/* ---- E1: Admin hardening ---- */

static int ena_get_dev_attr(struct ena_dev *d)
{
	struct ena_admin_aq_entry cmd;
	ena_feat_cmd(&cmd, ENA_ADMIN_OP_GET_FEATURE, ENA_ADMIN_FEAT_DEVICE_ATTRIBUTES);

	union {
		struct ena_admin_get_feat_dev_attr dev_attr;
		uint8_t raw[56];
	} resp_buf;
	kmemset(&resp_buf, 0, sizeof(resp_buf));
	int rc = ena_admin_submit(d, &cmd, &resp_buf);
	if (rc != 0) {
		kprintf("ena: GET_FEATURE(DEVICE_ATTRIBUTES) failed (status=%d)\n", rc);
		return -1;
	}

	for (int i = 0; i < 6; i++) d->mac[i] = resp_buf.dev_attr.mac_addr[i];
	d->max_mtu = resp_buf.dev_attr.max_mtu ? resp_buf.dev_attr.max_mtu : 1500;
	kprintf("ena: mac %02x:%02x:%02x:%02x:%02x:%02x  mtu %u\n",
		d->mac[0], d->mac[1], d->mac[2],
		d->mac[3], d->mac[4], d->mac[5],
		d->max_mtu);
	return 0;
}

static int ena_get_max_queues(struct ena_dev *d)
{
	struct ena_admin_aq_entry cmd;
	ena_feat_cmd(&cmd, ENA_ADMIN_OP_GET_FEATURE, ENA_ADMIN_FEAT_MAX_QUEUES_NUM);

	uint8_t resp[56];
	kmemset(resp, 0, sizeof(resp));
	int rc = ena_admin_submit(d, &cmd, resp);
	if (rc != 0) {
		kprintf("ena: GET_FEATURE(MAX_QUEUES) failed (status=%d), "
			"using defaults\n", rc);
		d->max_sq_depth = ENA_IOQ_DEPTH;
		return 0;
	}

	/* Response layout (ena_admin_get_feat_max_queue_ex):
	 *   [0..3]  max_sq_num      (u32)
	 *   [4..7]  max_sq_depth    (u32)
	 *   [8..11] max_cq_num      (u32)
	 *   [12..15] max_cq_depth   (u32) */
	uint32_t max_sq_depth = *(uint32_t *)(resp + 4);
	if (max_sq_depth == 0) max_sq_depth = ENA_IOQ_DEPTH;
	d->max_sq_depth = max_sq_depth < ENA_IOQ_DEPTH ?
			  max_sq_depth : ENA_IOQ_DEPTH;
	kprintf("ena: max_sq_depth %u (clamped to %u)\n",
		max_sq_depth, d->max_sq_depth);
	return 0;
}

static int ena_get_llq(struct ena_dev *d)
{
	struct ena_admin_aq_entry cmd;
	ena_feat_cmd(&cmd, ENA_ADMIN_OP_GET_FEATURE, ENA_ADMIN_FEAT_LLQ);

	uint8_t resp[56];
	kmemset(resp, 0, sizeof(resp));
	int rc = ena_admin_submit(d, &cmd, resp);
	(void)rc;	/* result is informational; we always use host memory */

	/* Always use host-memory placement (placement_policy=1).
	 * LLQ (policy=3) requires an additional push-queue header buffer
	 * that we have not set up.  Firmware rejects CREATE_SQ with
	 * ENA_SPEC_VERSION_MISMATCH (status=6) when told placement=3
	 * without the matching LLQ descriptor ring.  Host memory is
	 * functionally equivalent and simpler. */
	d->use_llq = 0;
	kprintf("ena: using host memory placement policy\n");
	return 0;
}

static int ena_set_host_attr(struct ena_dev *d)
{
	/* Allocate a 4 KB host-attributes page.
	 * Layout (ena_admin_host_info):
	 *   [0..3]   os_type   (u32): 4 = Generic/Other
	 *   [4..255] os_dist_str (252 bytes, NUL-terminated)
	 *   ...      rest zeroed
	 * Required by some Nitro firmware before I/O queue creation. */
	void *ha = pmm_alloc();
	if (!ha) {
		kprintf("ena: pmm_alloc failed for host attr\n");
		return -1;
	}
	kmemset(ha, 0, 4096);
	*(uint32_t *)ha = 4;	/* os_type = Generic */

	struct ena_admin_aq_entry cmd;
	ena_feat_cmd(&cmd, ENA_ADMIN_OP_SET_FEATURE, ENA_ADMIN_FEAT_HOST_ATTR_CONFIG);

	uint64_t ha_pa = (uint64_t)(uintptr_t)ha;
	/* SET_FEATURE(HOST_ATTR_CONFIG) payload:
	 *   [0..3]  ctrl_buff length (0 for inline)
	 *   [4..7]  ctrl_buff addr_lo
	 *   [8..9]  ctrl_buff addr_hi
	 *   [12..15] feat_common (already set by ena_feat_cmd)
	 *   [16..19] os_info_ba.addr_lo
	 *   [20..21] os_info_ba.addr_hi */
	*(uint32_t *)(cmd.payload + 16) = (uint32_t)ha_pa;
	*(uint16_t *)(cmd.payload + 20) = (uint16_t)(ha_pa >> 32);

	__asm__ volatile ("dc cvac, %0\n\t" "dsb sy\n\t" :: "r"(ha) : "memory");

	uint8_t resp[56];
	kmemset(resp, 0, sizeof(resp));
	int rc = ena_admin_submit(d, &cmd, resp);
	if (rc != 0)
		kprintf("ena: SET_FEATURE(HOST_ATTR_CONFIG) failed (status=%d), "
			"continuing\n", rc);
	else
		kprintf("ena: host attributes configured\n");

	/* Page can be freed now -- device has read it. */
	pmm_free(ha);
	return 0;
}

/* ---- I/O queue creation ---- */

static int ena_create_io_q(struct ena_dev *d, struct ena_io_q *q,
			   int direction)
{
	q->sq_pa = pmm_alloc();
	q->cq_pa = pmm_alloc();
	if (!q->sq_pa || !q->cq_pa) return -1;
	kmemset(q->sq_pa, 0, 4096);
	kmemset(q->cq_pa, 0, 4096);

	uint32_t depth = d->max_sq_depth ? d->max_sq_depth : ENA_IOQ_DEPTH;
	q->depth       = depth;
	q->sq_tail     = 0;
	q->cq_head     = 0;
	q->cq_phase    = 1;
	q->sq_phase    = 1;	/* SQ descriptors carry a phase bit too;
				 * pass 1 = phase 1, toggles on each wrap */

	struct ena_admin_aq_entry cmd;
	uint8_t resp[56];

	/* CREATE_CQ */
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.common.opcode = ENA_ADMIN_OP_CREATE_CQ;
	uint64_t cq_pa = (uint64_t)(uintptr_t)q->cq_pa;
	cmd.payload[0] = 0;			/* cq_caps_1: polling mode */
	/* TX cdesc = 2 words (8 B); RX cdesc = 4 words (16 B). */
	cmd.payload[1] = direction ? 4 : 2;
	*(uint16_t *)(cmd.payload + 2) = (uint16_t)depth;
	*(uint32_t *)(cmd.payload + 4) = 0;	/* msix_vector = none */
	*(uint32_t *)(cmd.payload + 8) = (uint32_t)cq_pa;
	*(uint16_t *)(cmd.payload + 12) = (uint16_t)(cq_pa >> 32);
	kmemset(resp, 0, sizeof(resp));
	{
		int rc = ena_admin_submit(d, &cmd, resp);
		if (rc != 0) {
			kprintf("ena: CREATE_CQ (%s) failed (status=%d)\n",
				direction ? "rx" : "tx", rc);
			return -1;
		}
	}
	uint16_t cq_id = *(uint16_t *)(resp + 0);
	/* CREATE_CQ response (ena_admin_acq_create_cq_resp_desc):
	 *   [0..1]  cq_idx
	 *   [2..3]  cq_actual_depth
	 *   [4..7]  numa_node_register_offset
	 *   [8..11] cq_head_db_register_offset (0 = no CQ doorbell)
	 * Firmware usually leaves the CQ head doorbell at 0 -- polling
	 * consumers don't need it.  Writers must guard on nonzero. */
	q->cq_db_off   = *(uint32_t *)(resp + 8);

	/* CREATE_SQ */
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.common.opcode = ENA_ADMIN_OP_CREATE_SQ;
	uint64_t sq_pa = (uint64_t)(uintptr_t)q->sq_pa;
	/* sq_identity byte: bits[7:5] = direction (TX=1→0x20, RX=2→0x40) */
	cmd.payload[0] = (uint8_t)((direction + 1) << 5);
	cmd.payload[1] = 0;
	/* sq_caps_2: bits[1:0] = placement_policy (1=host_mem, 3=LLQ) */
	cmd.payload[2] = d->use_llq ? 0x03 : 0x01;
	/* sq_caps_3: bit[0] = is_physically_contiguous (our PMM pages are) */
	cmd.payload[3] = 0x01;
	*(uint16_t *)(cmd.payload + 4) = cq_id;
	*(uint16_t *)(cmd.payload + 6) = (uint16_t)depth;
	*(uint32_t *)(cmd.payload + 8) = (uint32_t)sq_pa;
	*(uint16_t *)(cmd.payload + 12) = (uint16_t)(sq_pa >> 32);
	kmemset(resp, 0, sizeof(resp));
	{
		int rc = ena_admin_submit(d, &cmd, resp);
		if (rc != 0) {
			kprintf("ena: CREATE_SQ (%s) failed (status=%d)\n",
				direction ? "rx" : "tx", rc);
			return -1;
		}
	}
	/* CREATE_SQ response (ena_admin_acq_create_sq_resp_desc):
	 *   [0..1]  sq_idx
	 *   [2..3]  reserved
	 *   [4..7]  sq_doorbell_offset (byte offset from BAR0)
	 *   [8..11] llq_descriptors_offset (0 for host-memory placement)
	 * Verified on Nitro: reading resp+8 gave 0 ("tx sq_db=0x0"),
	 * which sent every doorbell write to BAR0+0 = ENA_REG_VERSION. */
	q->sq_db_off = *(uint32_t *)(resp + 4);
	kprintf("ena: %s sq_db=0x%x cq_db=0x%x\n",
		direction ? "rx" : "tx", q->sq_db_off, q->cq_db_off);
	return 0;
}

/* Write one RX descriptor at the next SQ tail position, handing
 * buffer `buf_slot` (index into rx_bufs) back to the device.
 * Does NOT ring the doorbell -- callers batch that. */
static void ena_rx_post(struct ena_dev *d, unsigned buf_slot)
{
	struct ena_io_q       *q    = &d->rx;
	struct ena_io_rx_desc *ring = q->sq_pa;
	struct ena_io_rx_desc *desc = &ring[q->sq_tail % q->depth];

	desc->length       = ENA_RX_BUF_BYTES;
	desc->reserved     = 0;
	desc->ctrl         = (uint8_t)(ENA_RX_DESC_FIRST | ENA_RX_DESC_LAST
	                             | ENA_RX_DESC_COMP_REQ
	                             | (q->sq_phase & ENA_RX_DESC_PHASE));
	desc->req_id       = (uint16_t)buf_slot;
	desc->reserved2    = 0;
	uint64_t pa        = (uint64_t)(uintptr_t)d->rx_bufs[buf_slot];
	desc->buff_addr_lo = (uint32_t)pa;
	desc->buff_addr_hi = (uint16_t)(pa >> 32);
	desc->reserved3    = 0;

	cache_clean_range(desc, sizeof(*desc));

	q->sq_tail++;
	if (q->sq_tail % q->depth == 0) q->sq_phase ^= 1;
}

/* Pre-fill the RX SQ with PMM-backed buffers. */
static int ena_rx_refill_all(struct ena_dev *d)
{
	for (uint32_t i = 0; i < d->rx.depth; i++) {
		void *b = pmm_alloc();
		if (!b) return -1;
		d->rx_bufs[i] = b;
		ena_rx_post(d, i);
	}
	w32(d->regs, d->rx.sq_db_off, d->rx.sq_tail);
	return 0;
}

/* ---- E2: Ethernet helpers ---- */

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xff);
}
static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >>  8);
	p[3] = (uint8_t) v;
}
static uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
	     | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* ---- E3: TX ring ---- */

static void ena_tx_drain(struct ena_dev *d)
{
	struct ena_io_q        *q     = &d->tx;
	struct ena_io_tx_cdesc *cring = q->cq_pa;

	for (;;) {
		struct ena_io_tx_cdesc *c = &cring[q->cq_head % q->depth];
		cache_inval_range(c, sizeof(*c));
		if ((c->flags & 1u) != q->cq_phase) break;

		uint16_t rid = c->req_id;
		unsigned slot = rid % ENA_IOQ_DEPTH;
		if (d->tx_bufs[slot]) {
			pmm_free(d->tx_bufs[slot]);
			d->tx_bufs[slot] = NULL;
		}
		q->cq_head++;
		if (q->cq_head % q->depth == 0) q->cq_phase ^= 1;
		if (q->cq_db_off)
			w32(d->regs, q->cq_db_off, q->cq_head);
	}
}

static int ena_eth_tx(struct ena_dev *d, const void *frame, unsigned len)
{
	if (!d->io_ready || len == 0 || len > ENA_RX_BUF_BYTES) return -1;

	void *buf = pmm_alloc();
	if (!buf) return -1;
	kmemcpy(buf, frame, len);
	/* Pad to the 60-byte Ethernet minimum (64 with FCS).  ARP
	 * replies are 42 bytes bare; short frames may be dropped by
	 * the device or fabric, and the EC2 reachability prober never
	 * hears us -- instance status sticks at "initializing". */
	if (len < 60) {
		kmemset((uint8_t *)buf + len, 0, 60 - len);
		len = 60;
	}
	cache_clean_range(buf, len);

	struct ena_io_q *q    = &d->tx;
	unsigned         slot = q->sq_tail % q->depth;
	uint16_t         rid  = (uint16_t)slot;	/* req_id == ring slot */
	d->tx_bufs[slot] = buf;

	/* req_id rides split across the two control words:
	 * bits [10:5] -> len_ctrl[21:16], bits [4:0] -> meta_ctrl[26:22]. */
	struct ena_io_tx_desc *ring = q->sq_pa;
	struct ena_io_tx_desc *desc = &ring[slot];
	uint64_t pa = (uint64_t)(uintptr_t)buf;
	desc->len_ctrl = (len & 0xffffu)
	               | (((uint32_t)(rid >> 5) & 0x3fu) << 16)
	               | (q->sq_phase ? ENA_TX_DESC_PHASE : 0)
	               | ENA_TX_DESC_FIRST | ENA_TX_DESC_LAST
	               | ENA_TX_DESC_COMP_REQ;
	desc->meta_ctrl        = ((uint32_t)(rid & 0x1fu) << 22);
	desc->buff_addr_lo     = (uint32_t)pa;
	desc->buff_addr_hi_hdr = (uint32_t)((pa >> 32) & 0xffffu);

	cache_clean_range(desc, sizeof(*desc));

	q->sq_tail++;
	if (q->sq_tail % q->depth == 0) q->sq_phase ^= 1;
	w32(d->regs, q->sq_db_off, q->sq_tail);

	ena_tx_drain(d);
	return 0;
}

/* ---- E2: ARP (shared cache trampoline) ---- */

static int ena_arp_tx(void *cookie, const void *frame, unsigned len)
{
	return ena_eth_tx((struct ena_dev *)cookie, frame, len);
}

/* ---- E5: DHCP ---- */

static uint16_t dhcp_udp_checksum(uint32_t src_ip, uint32_t dst_ip,
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

static void dhcp_send(struct ena_dev *d, uint8_t msg_type,
		      uint32_t req_ip, uint32_t server_id)
{
	uint8_t frame[ENA_ETH_HDR_LEN + IPV4_HDR_LEN + UDP_HDR_LEN
		      + sizeof(struct dhcp_pkt)];
	kmemset(frame, 0, sizeof(frame));

	struct ena_ether_hdr *eh = (struct ena_ether_hdr *)frame;
	kmemset(eh->dst, 0xff, 6);
	kmemcpy(eh->src, d->mac, 6);
	put_be16((uint8_t *)&eh->ethertype, ENA_ETHERTYPE_IPV4);

	uint8_t *ip = frame + ENA_ETH_HDR_LEN;
	unsigned ip_total = IPV4_HDR_LEN + UDP_HDR_LEN + sizeof(struct dhcp_pkt);
	ip[0] = 0x45;
	ip[1] = 0;
	put_be16(ip + 2, (uint16_t)ip_total);
	put_be16(ip + 4, d->dhcp_xid & 0xffff);
	put_be16(ip + 6, 0);
	ip[8] = 64;
	ip[9] = IPPROTO_UDP;
	/* src = 0.0.0.0, dst = 255.255.255.255 */
	put_be32(ip + 12, 0);
	put_be32(ip + 16, 0xffffffff);
	uint16_t iphdr_csum = ip_checksum(ip, IPV4_HDR_LEN);
	put_be16(ip + 10, iphdr_csum);

	uint8_t *udp = ip + IPV4_HDR_LEN;
	put_be16(udp + 0, DHCP_CLIENT_PORT);
	put_be16(udp + 2, DHCP_SERVER_PORT);
	unsigned udp_total = UDP_HDR_LEN + sizeof(struct dhcp_pkt);
	put_be16(udp + 4, (uint16_t)udp_total);

	struct dhcp_pkt *dp = (struct dhcp_pkt *)(udp + UDP_HDR_LEN);
	dp->op    = DHCP_OP_BOOTREQUEST;
	dp->htype = DHCP_HTYPE_ETHER;
	dp->hlen  = DHCP_HLEN_ETHER;
	dp->xid   = (uint32_t)__builtin_bswap32(d->dhcp_xid);
	dp->flags = (uint16_t)((0x80 << 8) | 0x00);  /* broadcast flag BE */
	kmemcpy(dp->chaddr, d->mac, 6);
	dp->magic = (uint32_t)__builtin_bswap32(DHCP_MAGIC);

	uint8_t *opt = dp->options;
	*opt++ = DHCP_OPT_MSG_TYPE;  *opt++ = 1; *opt++ = msg_type;
	if (msg_type == DHCP_MSG_REQUEST) {
		*opt++ = DHCP_OPT_REQ_IP;  *opt++ = 4;
		put_be32(opt, req_ip); opt += 4;
		*opt++ = DHCP_OPT_SERVER_ID; *opt++ = 4;
		put_be32(opt, server_id); opt += 4;
	}
	*opt++ = DHCP_OPT_PARAM_LIST; *opt++ = 2;
	*opt++ = DHCP_OPT_SUBNET; *opt++ = DHCP_OPT_ROUTER;
	*opt++ = DHCP_OPT_END;

	uint16_t udp_csum = dhcp_udp_checksum(0, 0xffffffff, udp, udp_total);
	put_be16(udp + 6, udp_csum);

	ena_eth_tx(d, frame, sizeof(frame));
}

/* Returns 1 if this frame was consumed by the DHCP state machine. */
static int dhcp_rx_filter(struct ena_dev *d,
			  const uint8_t *frame, unsigned len)
{
	if (d->dhcp_state == DHCP_BOUND || d->dhcp_state == DHCP_FAILED)
		return 0;

	/* Need Ethernet + IP + UDP + the fixed BOOTP header through the
	 * magic cookie (240 bytes).  Do NOT require the full 312-byte
	 * options field: QEMU's slirp pads its replies out to the
	 * maximum, but real servers -- the EC2 VPC DHCP responder in
	 * particular -- send compact frames (~300 bytes total) carrying
	 * only the options they need.  Requiring sizeof(struct
	 * dhcp_pkt) here silently rejected every real OFFER. */
	unsigned min = ENA_ETH_HDR_LEN + IPV4_HDR_LEN + UDP_HDR_LEN + 240;
	if (len < min) return 0;

	const uint8_t *ip  = frame + ENA_ETH_HDR_LEN;
	if ((ip[0] & 0xf0) != 0x40) return 0;
	if (ip[9] != IPPROTO_UDP) return 0;

	const uint8_t *udp = ip + IPV4_HDR_LEN;
	if (get_be16(udp + 2) != DHCP_CLIENT_PORT) return 0;

	const struct dhcp_pkt *dp =
		(const struct dhcp_pkt *)(udp + UDP_HDR_LEN);
	if (__builtin_bswap32(dp->magic) != DHCP_MAGIC) return 0;
	if (__builtin_bswap32(dp->xid) != d->dhcp_xid) return 0;
	if (dp->op != DHCP_OP_BOOTREPLY) return 0;

	/* Parse options for DHCP message type.  Bound the walk by the
	 * received frame, not the struct's full options capacity. */
	uint8_t msg_type = 0;
	uint32_t subnet = 0, router = 0, server = 0;
	const uint8_t *o = dp->options;
	const uint8_t *oend = frame + len;
	if (oend > dp->options + sizeof(dp->options))
		oend = dp->options + sizeof(dp->options);
	while (o < oend && *o != DHCP_OPT_END) {
		uint8_t code = *o++;
		if (code == DHCP_OPT_PAD) continue;
		if (o >= oend) break;
		uint8_t olen = *o++;
		if (o + olen > oend) break;
		if (code == DHCP_OPT_MSG_TYPE && olen >= 1)
			msg_type = o[0];
		else if (code == DHCP_OPT_SUBNET && olen >= 4)
			subnet = get_be32(o);
		else if (code == DHCP_OPT_ROUTER && olen >= 4)
			router = get_be32(o);
		else if (code == DHCP_OPT_SERVER_ID && olen >= 4)
			server = get_be32(o);
		o += olen;
	}

	uint32_t offered_ip = __builtin_bswap32(dp->yiaddr);

	if (msg_type == DHCP_MSG_OFFER &&
	    d->dhcp_state == DHCP_WAIT_OFFER) {
		d->dhcp_offer_ip      = offered_ip;
		d->dhcp_offer_netmask = subnet ? subnet : 0xffffff00u;
		d->dhcp_offer_gw      = router;
		d->dhcp_offer_server  = server;
		d->dhcp_state         = DHCP_WAIT_ACK;
		dhcp_send(d, DHCP_MSG_REQUEST,
			  d->dhcp_offer_ip, d->dhcp_offer_server);
		return 1;
	}

	if (msg_type == DHCP_MSG_ACK &&
	    d->dhcp_state == DHCP_WAIT_ACK) {
		d->ip       = offered_ip ? offered_ip : d->dhcp_offer_ip;
		d->netmask  = subnet ? subnet : d->dhcp_offer_netmask;
		d->gateway  = router ? router : d->dhcp_offer_gw;
		d->dhcp_state = DHCP_BOUND;
		return 1;
	}

	return 1;
}

/* ---- E4: RX path ---- */

static void ena_rx_dispatch(struct ena_dev *d,
			    const uint8_t *frame, unsigned len)
{
	if (len < ENA_ETH_HDR_LEN) return;
	const struct ena_ether_hdr *eh = (const struct ena_ether_hdr *)frame;
	uint16_t et = get_be16((const uint8_t *)&eh->ethertype);

	/* Capture the first frame that arrives while DHCP is unbound.
	 * If DHCP later fails, this is hexdumped -- either it's the
	 * reply the filter mis-rejected (and the dump says why), or
	 * it's ambient broadcast proving our DISCOVER never made it
	 * out.  Consumed replies never print (DHCP succeeds). */
	if ((d->dhcp_state == DHCP_WAIT_OFFER ||
	     d->dhcp_state == DHCP_WAIT_ACK) && d->dbg_rx_len == 0) {
		d->dbg_rx_len = len < sizeof(d->dbg_rx) ? len : sizeof(d->dbg_rx);
		kmemcpy(d->dbg_rx, frame, d->dbg_rx_len);
	}

	if (et == ENA_ETHERTYPE_ARP) {
		arp_input(&d->aif, frame, len);
		return;
	}

	if (et != ENA_ETHERTYPE_IPV4) return;

	if (dhcp_rx_filter(d, frame, len)) return;

	if (d->ena_sd && d->ena_sd->sd_drv_rq) {
		unsigned plen = len - ENA_ETH_HDR_LEN;
		mblk_t *mp = allocb(plen, 0);
		if (mp) {
			kmemcpy(mp->b_wptr, frame + ENA_ETH_HDR_LEN, plen);
			mp->b_wptr += plen;
			mp->b_datap->db_type = M_DATA;
			queue_t *rq = d->ena_sd->sd_drv_rq;
			if (rq && rq->q_next)
				putnext(rq, mp);
			else
				freemsg(mp);
		}
	}
}

/* Poll one RX CQ entry.  Returns 1 if a packet was processed, 0 if ring empty. */
static int ena_rx_poll_one(struct ena_dev *d)
{
	struct ena_io_q        *q     = &d->rx;
	struct ena_io_rx_cdesc *cring = q->cq_pa;
	struct ena_io_rx_cdesc *c     = &cring[q->cq_head % q->depth];

	cache_inval_range(c, sizeof(*c));
	if (((c->status & ENA_RX_CDESC_PHASE) ? 1u : 0u) != q->cq_phase)
		return 0;

	uint16_t rid   = c->req_id;
	uint16_t rxlen = c->length;
	unsigned slot  = rid % ENA_IOQ_DEPTH;
	void    *buf   = d->rx_bufs[slot];

	if (rxlen > 0 && rxlen <= ENA_RX_BUF_BYTES && buf) {
		cache_inval_range(buf, rxlen);
		ena_rx_dispatch(d, (const uint8_t *)buf, rxlen);
	}

	q->cq_head++;
	if (q->cq_head % q->depth == 0) q->cq_phase ^= 1;
	if (q->cq_db_off)
		w32(d->regs, q->cq_db_off, q->cq_head);

	/* Hand the buffer straight back to the device. */
	ena_rx_post(d, slot);
	w32(d->regs, q->sq_db_off, q->sq_tail);
	return 1;
}

static void ena_rx_thread(void *arg)
{
	struct ena_dev *d = arg;
	for (;;) {
		int drained = 0;
		while (ena_rx_poll_one(d)) drained++;
		if (!drained) kthread_yield();
	}
}

/* ---- E5: DHCP run ---- */

/* Poll RX + reap TX until dhcp_state leaves `from_state` or `ms`
 * elapses.  Returns 1 when the state moved on. */
static int ena_dhcp_wait(struct ena_dev *d, int from_state, unsigned ms)
{
	uint64_t freq, start, deadline;
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
	deadline = start + (freq / 1000ULL) * (uint64_t)ms;

	while (d->dhcp_state == from_state) {
		ena_rx_poll_one(d);
		ena_tx_drain(d);	/* reap bounce buffers + keep the
					 * done-counter honest for the
					 * failure diagnostic */
		kthread_yield();
		uint64_t now;
		__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
		if (now >= deadline) return 0;
	}
	return 1;
}

static int ena_dhcp_run(struct ena_dev *d)
{
	d->dhcp_xid = 0xd1a10001u;	/* fixed seed; fine for single NIC */

	/* DISCOVER, 3 attempts x 2 s.  The EC2 DHCP responder is fast
	 * (<10 ms) but the first broadcast can be dropped while the NIC
	 * settles after queue creation. */
	for (int attempt = 0; attempt < 3; attempt++) {
		d->dhcp_state = DHCP_WAIT_OFFER;
		dhcp_send(d, DHCP_MSG_DISCOVER, 0, 0);
		if (ena_dhcp_wait(d, DHCP_WAIT_OFFER, 2000))
			break;
	}
	if (d->dhcp_state != DHCP_WAIT_ACK) {
		d->dhcp_state = DHCP_FAILED;
		return 0;
	}

	/* dhcp_rx_filter already sent REQUEST on the OFFER; wait for the
	 * ACK, retransmitting the REQUEST on timeout. */
	for (int attempt = 0; attempt < 3; attempt++) {
		if (ena_dhcp_wait(d, DHCP_WAIT_ACK, 2000))
			break;
		dhcp_send(d, DHCP_MSG_REQUEST,
			  d->dhcp_offer_ip, d->dhcp_offer_server);
	}
	if (d->dhcp_state != DHCP_BOUND)
		d->dhcp_state = DHCP_FAILED;
	return d->dhcp_state == DHCP_BOUND;
}

/* ---- E6: STREAMS personality ---- */

static int ena_wq_putp(queue_t *q, mblk_t *mp)
{
	(void)q;
	if (!mp) return 0;
	if (mp->b_datap->db_type != M_DATA) { freemsg(mp); return 0; }

	unsigned plen = (unsigned)msgdsize(mp);
	if (plen == 0 || plen > (unsigned)g_ena.nif.mtu) {
		freemsg(mp); return -1;
	}

	/* Next hop: on-subnet destinations get ARPed directly, anything
	 * else goes via the gateway.  Datagram dst is at IP header
	 * bytes 16..19 (ip_wput builds the header contiguously). */
	uint8_t dmac[6];
	{
		const uint8_t *iph = mp->b_rptr;
		uint32_t dst = get_be32(iph + 16);
		if (dst == 0xffffffffu) {
			kmemset(dmac, 0xff, 6);
		} else {
			/* Addressing lives on the netif -- runtime plumbing
			 * (SIOCSIF*) changes it there, not in driver fields. */
			struct netif *nif = &g_ena.nif;
			uint32_t nh = ((dst & nif->netmask) ==
			               (nif->ip & nif->netmask)) ? dst
			                                         : nif->gateway;
			if (!arp_resolve(&g_ena.aif, nh, dmac)) {
				/* Request on the wire; drop, upper layers
				 * retransmit. */
				freemsg(mp);
				return 0;
			}
		}
	}

	uint8_t frame[ENA_ETH_HDR_LEN + 1500];
	struct ena_ether_hdr *eh = (struct ena_ether_hdr *)frame;
	kmemcpy(eh->dst, dmac, 6);
	kmemcpy(eh->src, g_ena.mac, 6);
	put_be16((uint8_t *)&eh->ethertype, ENA_ETHERTYPE_IPV4);

	unsigned char *p = frame + ENA_ETH_HDR_LEN;
	for (mblk_t *m = mp; m; m = m->b_cont) {
		unsigned n = (unsigned)(m->b_wptr - m->b_rptr);
		if (n) { kmemcpy(p, m->b_rptr, n); p += n; }
	}
	freemsg(mp);

	return ena_eth_tx(&g_ena, frame, ENA_ETH_HDR_LEN + plen);
}

static int ena_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static int ena_qopen(queue_t *rq)
{
	(void)rq;
	return 0;
}

static struct module_info ena_minfo = {
	.mi_idnum  = 2001,
	.mi_idname = "ena0",
	.mi_minpsz = 0,
	.mi_maxpsz = 1500,
	.mi_hiwat  = 32768,
	.mi_lowat  = 8192,
};

static struct qinit ena_rinit = {
	.qi_putp  = ena_rq_putp,
	.qi_qopen = ena_qopen,
	.qi_minfo = &ena_minfo,
};
static struct qinit ena_winit = {
	.qi_putp  = ena_wq_putp,
	.qi_minfo = &ena_minfo,
};
static struct streamtab ena_streamtab = {
	.st_rdinit = &ena_rinit,
	.st_wrinit = &ena_winit,
};

/* ---- E7: IP wire-up ---- */

static void wire_ena_into_ip(struct ena_dev *d)
{
	struct stdata *sd = stream_build_kernel(&ena_streamtab, "ena0_drv", 0);
	if (!sd) {
		kprintf("ena: stream_build_kernel failed\n");
		return;
	}
	d->ena_sd = sd;

	d->nif.name      = "eth0";
	d->nif.ip        = d->ip;
	d->nif.netmask   = d->netmask;
	d->nif.gateway   = d->gateway;
	d->nif.mac       = d->mac;
	d->nif.mtu       = (d->max_mtu < 1500) ? d->max_mtu : 1500;
	d->nif.streamtab = NULL;	/* pre-built stream via ip_attach_stream */
	d->nif.tx        = NULL;
	netif_register(&d->nif);

	long muxid = ip_attach_stream(sd, &d->nif);
	if (muxid <= 0) {
		kprintf("ena: ip_attach_stream failed rc=%ld\n", muxid);
		return;
	}
	kprintf("ena: linked under IP muxid=%ld\n", muxid);
}

/* ---- Probe + top-level init ---- */

static struct pci_device *ena_find(void)
{
	for (uint32_t i = 0; i < pci_nr_devs; i++) {
		struct pci_device *p = &pci_devs[i];
		if (p->vendor_id != ENA_PCI_VID) continue;
		if (p->device_id != ENA_PCI_DID_EC20 &&
		    p->device_id != ENA_PCI_DID_EC21) continue;
		if (p->class_subclass != ENA_PCI_CLASS_NET) continue;
		return p;
	}
	return NULL;
}

void ena_init(void)
{
	struct pci_device *p = ena_find();
	if (!p) return;

	struct ena_dev *d = &g_ena;
	kmemset(d, 0, sizeof(*d));

	volatile uint16_t *cfg_cmd = (volatile uint16_t *)
		(uintptr_t)(pcie_ecam_base + ((uint64_t)p->bus << 20) +
			    ((uint64_t)p->dev << 15) +
			    ((uint64_t)p->fn  << 12) + 0x04);
	*cfg_cmd = (uint16_t)(*cfg_cmd | 0x0006);

	uint64_t bar0_pa = pcie_bar_addr(p, 0);
	if (!bar0_pa) {
		kprintf("ena: BAR0 not a memory BAR\n");
		return;
	}
	mmu_map_device_1gb(bar0_pa);
	d->regs = (volatile uint8_t *)(uintptr_t)bar0_pa;

	ena_mmio_read_init(d);

	uint32_t ver  = ena_r32(d, ENA_REG_VERSION);
	uint32_t cver = ena_r32(d, ENA_REG_CONTROLLER_VERSION);
	uint32_t sts  = ena_r32(d, ENA_REG_DEV_STS);
	kprintf("ena: bar0 0x%lx  ver 0x%x  ctrl-ver 0x%x  sts 0x%x\n",
		bar0_pa, ver, cver, sts);

	if (ena_reset(d)           < 0) return;
	if (ena_admin_init(d)      < 0) return;
	if (ena_get_dev_attr(d)    < 0) return;
	if (ena_get_max_queues(d)  < 0) return;	/* E1 */
	ena_get_llq(d);			        /* E1: failure is non-fatal */
	ena_set_host_attr(d);		        /* E1: failure is non-fatal */
	if (ena_create_io_q(d, &d->tx, 0) < 0) return;
	if (ena_create_io_q(d, &d->rx, 1) < 0) return;
	if (ena_rx_refill_all(d)   < 0) return;

	/* io_ready gates ena_eth_tx; must be set before DHCP so
	 * dhcp_send can actually transmit DISCOVER/REQUEST frames. */
	d->io_ready = 1;

	/* Shared ARP cache identity: live before the first RX poll so
	 * arp_input can learn from broadcasts during DHCP. */
	d->nif.name  = "eth0";	/* re-set at netif_register */
	d->aif.nif    = &d->nif;
	d->aif.mac    = d->mac;
	d->aif.tx     = ena_arp_tx;
	d->aif.cookie = d;
	arp_ifattach(&d->aif);

	/* E5: DHCP (spin-polls RX CQ; kthread_yield lets other threads run).
	 * wire_ena_into_ip requires ip_ctl_sd which is set by ip_init();
	 * ip_init() runs after us in main.c -- guard on DHCP success so
	 * we don't attempt wire-up before ip_ctl_sd exists. */
	if (ena_dhcp_run(d)) {
		unsigned nm_bits = 0;
		uint32_t m = d->netmask;
		while (m) { nm_bits += m & 1u; m >>= 1; }
		kprintf("ena: DHCP bound %u.%u.%u.%u/%u gw %u.%u.%u.%u\n",
			(d->ip >> 24) & 0xff, (d->ip >> 16) & 0xff,
			(d->ip >>  8) & 0xff,  d->ip        & 0xff,
			nm_bits,
			(d->gateway >> 24) & 0xff, (d->gateway >> 16) & 0xff,
			(d->gateway >>  8) & 0xff,  d->gateway        & 0xff);

		/* E6+E7: STREAMS wire-up (requires ip_ctl_sd from ip_init).
		 * When ena_init is called before ip_init, ip_attach_stream
		 * returns -1 safely; the caller (main.c's PLATFORM_VIRT block)
		 * should call ena_streams_attach() again after ip_init. */
		wire_ena_into_ip(d);
		if (d->ena_sd) {
			g_ena_init_ok = 1;
			kprintf("ena: ready  mac %02x:%02x:%02x:%02x:%02x:%02x"
				"  mtu %u  eth0 linked\n",
				d->mac[0], d->mac[1], d->mac[2],
				d->mac[3], d->mac[4], d->mac[5],
				d->nif.mtu);
			/* E4: start RX poll thread only after IP is wired */
			kthread_create("ena_rx", ena_rx_thread, d);
		}
	} else {
		kprintf("ena: DHCP failed -- no IP assigned, network inactive\n");
		/* Field diagnostic: tx done > 0 means the device consumed
		 * our DISCOVER (TX descriptor format accepted); rx got > 0
		 * means replies arrived but the DHCP filter didn't match. */
		kprintf("ena: tx sent=%u done=%u  rx got=%u\n",
			d->tx.sq_tail, d->tx.cq_head, d->rx.cq_head);
		if (d->dbg_rx_len) {
			kprintf("ena: first unmatched rx (%u B):", d->dbg_rx_len);
			for (unsigned i = 0; i < d->dbg_rx_len; i++)
				kprintf("%s%02x", (i % 16) ? " " : "\n  ",
					d->dbg_rx[i]);
			kprintf("\n");
		}
	}
}

int ena_present(void)
{
	return ena_find() != NULL;
}

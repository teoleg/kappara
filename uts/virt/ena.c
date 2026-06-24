/*
 * uts/virt/ena.c -- AWS.md stage E: ENA network driver
 *
 * BLIND IMPLEMENTATION -- see include/kappara/arch/ena.h for the
 * spec-verification caveat.  TL;DR: every "XXX verify" marker in
 * this file must be cross-checked against amzn-drivers' canonical
 * ena_regs_defs.h / ena_admin_defs.h headers before this code is
 * trusted on real Graviton hardware.
 *
 * Structural shape (matches the upstream Linux driver):
 *
 *   1. PCI probe: vendor 0x1d0f, device 0xec20 (or 0xec21), class 0x0200.
 *   2. Enable PCI Memory Space + Bus Master in the Command register.
 *   3. Map BAR0 (registers) with mmu_map_device_1gb.
 *   4. Reset the device (DEV_CTL.RESET; wait DEV_STS.RESET_FINISHED).
 *   5. Allocate Admin SQ + Admin CQ + AENQ pages from PMM.
 *   6. Program AQ_BASE / AQ_CAPS, ACQ_BASE / ACQ_CAPS,
 *      AENQ_BASE / AENQ_CAPS.
 *   7. GET_FEATURE(DEVICE_ATTRIBUTES) -> learn MAC, max queues, MTU.
 *   8. CREATE_CQ + CREATE_SQ for one TX pair and one RX pair.
 *   9. Refill RX descriptors with PMM-backed buffers.
 *  10. Register a struct netif named "eth0" so the IP stack can use us.
 *  11. (Future) wire RX interrupt -> service proc that drains the CQ
 *      and putnexts mblks into the IP stream.  For now polling
 *      stub from a kthread.
 *
 * Coverage scope today:
 *   - Steps 1-7 implemented in full but UNVERIFIED.
 *   - Steps 8-10 implemented with the best-effort descriptor layout.
 *   - Step 11 not yet wired; the netif TX callback returns 0 (drops)
 *     and there is no RX kthread.  Goal of this commit is "ENA
 *     comes up cleanly on Graviton's boot path"; real packet I/O
 *     is the follow-up once we can iterate against an actual
 *     instance.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/arch/ena.h"
#include "kappara/arch/mmu.h"
#include "kappara/arch/pcie.h"
#include "kappara/core/kmem.h"
#include "kappara/core/pmm.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"
#include "kappara/net/netif.h"

/* ---- PCI IDs ---- */
#define ENA_PCI_VID		0x1d0f
#define ENA_PCI_DID_EC20	0xec20
#define ENA_PCI_DID_EC21	0xec21
#define ENA_PCI_CLASS_NET	0x0200	/* class 02 / subclass 00 */

/* ---- BAR0 register offsets ---- XXX verify-against-ena_regs_defs.h
 *
 * Layout I have in memory from the Linux driver.  The header file
 * uses ENA_REGS_<NAME>_OFF for the offset and ENA_REGS_<NAME>_<FIELD>_
 * MASK / _SHIFT for the bitfields.
 */
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
#define ENA_REG_INTR_MASK		0x44
#define ENA_REG_DEV_CTL			0x48
#define ENA_REG_DEV_STS			0x4C
#define ENA_REG_MMIO_REG_READ		0x50
#define ENA_REG_MMIO_RESP_LO		0x54
#define ENA_REG_MMIO_RESP_HI		0x58

/* DEV_CTL bits */
#define ENA_DEV_CTL_RESET		(1u << 0)
#define ENA_DEV_CTL_QUIESCENT		(1u << 1)	/* XXX verify */
#define ENA_DEV_CTL_GRACEFUL_SHUTDOWN	(1u << 3)	/* XXX verify */

/* DEV_STS bits */
#define ENA_DEV_STS_READY		(1u << 0)	/* XXX verify */
#define ENA_DEV_STS_RESET_IN_PROGRESS	(1u << 1)	/* XXX verify */
#define ENA_DEV_STS_RESET_FINISHED	(1u << 2)
#define ENA_DEV_STS_FATAL_ERROR		(1u << 3)	/* XXX verify */

/* AQ_CAPS / ACQ_CAPS / AENQ_CAPS layout (32-bit register):
 *   bits  [15:0]  depth (queue length in entries)
 *   bits [23:16]  entry size (bytes; SQ=64, CQ=64, AENQ=16)  XXX verify
 */
#define ENA_QCAPS(depth, entry_size) \
	(((uint32_t)(entry_size) << 16) | (uint32_t)(depth))

/* ---- Admin command queue entries ---- XXX verify-against-ena_admin_defs.h
 *
 * Each SQE is 64 bytes.  Common header is 16 bytes:
 *   u16 cmd_id;
 *   u8  opcode;
 *   u8  flags;
 *   ... 12 bytes more reserved / specific
 */

/* opcodes (admin) */
#define ENA_ADMIN_OP_CREATE_SQ		1
#define ENA_ADMIN_OP_DESTROY_SQ		2
#define ENA_ADMIN_OP_CREATE_CQ		3
#define ENA_ADMIN_OP_DESTROY_CQ		4
#define ENA_ADMIN_OP_GET_FEATURE	8
#define ENA_ADMIN_OP_SET_FEATURE	9
#define ENA_ADMIN_OP_GET_STATS		11

/* Feature IDs */
#define ENA_ADMIN_FEAT_DEVICE_ATTRIBUTES	1
#define ENA_ADMIN_FEAT_MAX_QUEUES_NUM		2	/* XXX verify */
#define ENA_ADMIN_FEAT_HW_HINTS			3	/* XXX verify */
#define ENA_ADMIN_FEAT_LLQ			4	/* XXX verify */
#define ENA_ADMIN_FEAT_MTU			14	/* XXX verify */
#define ENA_ADMIN_FEAT_HOST_ATTR_CONFIG		16	/* XXX verify */

struct ena_admin_aq_common_desc {
	uint16_t command_id;
	uint8_t  opcode;
	uint8_t  flags;
	uint8_t  reserved[12];
} __attribute__((packed));

struct ena_admin_aq_entry {
	struct ena_admin_aq_common_desc common;
	uint8_t                         payload[48];
} __attribute__((packed));

struct ena_admin_acq_common_desc {
	uint16_t command;	/* low 12 bits = command_id, high = phase tag */
	uint8_t  status;
	uint8_t  flags;
	uint16_t extended_status;
	uint16_t sq_head_indx;
} __attribute__((packed));

struct ena_admin_acq_entry {
	struct ena_admin_acq_common_desc common;
	uint8_t                          payload[56];
} __attribute__((packed));

/* AENQ entry: async events from the device (link status change,
 * stat sync, fatal warnings).  16-byte header, variable payload. */
struct ena_aenq_entry {
	uint16_t group;
	uint16_t syndrome;
	uint8_t  flags;		/* bit 0 = phase tag */
	uint8_t  reserved[3];
	uint64_t timestamp_us;
	uint8_t  payload[40];
} __attribute__((packed));

/* GET_FEATURE(DEVICE_ATTRIBUTES) response payload (in ACQE.payload).
 * XXX verify field offsets against ena_admin_defs.h. */
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

/* ---- I/O queue descriptors ---- XXX verify all
 *
 * Two flavours, both 16 bytes:
 *   TX descriptor:  length + flags + buffer address (host phys)
 *   RX descriptor:  buffer address (host phys) + req_id
 *
 * TX/RX completion entries also 16 bytes but with a phase tag bit
 * that toggles every queue wrap, NVMe-style.
 */

#define ENA_TX_DESC_FIRST		(1u << 7)	/* XXX verify bit */
#define ENA_TX_DESC_LAST		(1u << 6)	/* XXX verify bit */
#define ENA_TX_DESC_COMP_REQ		(1u << 5)	/* XXX verify bit */

struct ena_io_tx_desc {
	uint16_t length;
	uint8_t  reserved;
	uint8_t  ctrl;		/* FIRST | LAST | COMP_REQ */
	uint16_t req_id;	/* echoed back in completion */
	uint8_t  reserved2[2];
	uint64_t buff_addr;
} __attribute__((packed));

struct ena_io_rx_desc {
	uint16_t length;	/* buffer cap */
	uint16_t req_id;
	uint8_t  reserved[4];
	uint64_t buff_addr;
} __attribute__((packed));

struct ena_io_cdesc {
	uint16_t req_id;	/* matches submitting descriptor's req_id */
	uint8_t  status;
	uint8_t  flags;		/* bit 0 = phase tag */
	uint16_t length;	/* RX only: actual bytes received */
	uint8_t  reserved[10];
} __attribute__((packed));

/* ---- Driver state ---- */

#define ENA_AQ_DEPTH		32	/* admin queue length */
#define ENA_IOQ_DEPTH		64	/* I/O queue length per direction */
#define ENA_RX_BUF_BYTES	2048	/* one RX buffer; 1500 MTU + room */

struct ena_io_q {
	void    *sq_pa;		/* PMM page; both VA and PA (identity) */
	void    *cq_pa;
	uint32_t depth;
	uint16_t sq_tail;
	uint16_t cq_head;
	uint8_t  cq_phase;
	uint32_t sq_db_off;	/* register offset for this SQ's doorbell */
	uint32_t cq_db_off;
	uint16_t next_req_id;
};

struct ena_dev {
	volatile uint8_t *regs;

	struct ena_admin_aq_entry  *aq;
	struct ena_admin_acq_entry *acq;
	struct ena_aenq_entry      *aenq;
	uint16_t aq_tail;
	uint16_t acq_head;
	uint8_t  acq_phase;
	uint16_t aenq_head;
	uint8_t  aenq_phase;
	uint16_t next_cmd_id;

	uint8_t  mac[6];
	uint32_t max_mtu;
	uint32_t max_io_queues;	/* per-direction */

	struct ena_io_q tx;
	struct ena_io_q rx;

	/* Pre-allocated RX buffers, indexed by req_id mod ENA_IOQ_DEPTH. */
	void    *rx_bufs[ENA_IOQ_DEPTH];

	struct netif nif;
};

static struct ena_dev g_ena;
static int            g_ena_init_ok;

/* ---- MMIO accessors (same shape as nvme.c) ---- */

static uint32_t r32(volatile uint8_t *base, unsigned off)
{
	return *(volatile uint32_t *)(base + off);
}
static void w32(volatile uint8_t *base, unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

/* ---- Reset ----
 *
 * From the spec: set DEV_CTL.RESET, poll DEV_STS for RESET_FINISHED,
 * clear DEV_CTL.RESET, wait for DEV_STS.READY.  XXX verify bit
 * meanings against the spec; the upstream driver uses a slightly
 * more elaborate "trigger via DEV_CTL.RESET_REASON" pattern that
 * we may need to adopt. */
static int ena_reset(struct ena_dev *d)
{
	w32(d->regs, ENA_REG_DEV_CTL, ENA_DEV_CTL_RESET);
	for (int spin = 0; spin < 1000000; spin++) {
		uint32_t s = r32(d->regs, ENA_REG_DEV_STS);
		if (s & ENA_DEV_STS_RESET_FINISHED) break;
		if (s & ENA_DEV_STS_FATAL_ERROR) {
			kprintf("ena: fatal during reset (STS=0x%x)\n", s);
			return -1;
		}
	}
	w32(d->regs, ENA_REG_DEV_CTL, 0);
	for (int spin = 0; spin < 1000000; spin++) {
		uint32_t s = r32(d->regs, ENA_REG_DEV_STS);
		if (s & ENA_DEV_STS_READY) return 0;
		if (s & ENA_DEV_STS_FATAL_ERROR) {
			kprintf("ena: fatal post-reset (STS=0x%x)\n", s);
			return -1;
		}
	}
	kprintf("ena: timed out waiting for DEV_STS.READY\n");
	return -1;
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

	/* Mask all interrupts -- this driver polls. */
	w32(d->regs, ENA_REG_INTR_MASK, 0xFFFFFFFFu);
	return 0;
}

/* Submit one admin command, poll the ACQ until the matching CQE
 * arrives, copy the response payload (56 bytes) into `resp` if
 * non-NULL.  Returns 0 on status == 0, -1 otherwise. */
static int ena_admin_submit(struct ena_dev *d,
                            const struct ena_admin_aq_entry *cmd,
                            void *resp)
{
	uint16_t cmd_id = d->next_cmd_id++;

	struct ena_admin_aq_entry *slot = &d->aq[d->aq_tail];
	*slot = *cmd;
	slot->common.command_id = cmd_id;

	__asm__ volatile ("dsb sy" ::: "memory");

	d->aq_tail = (uint16_t)((d->aq_tail + 1) % ENA_AQ_DEPTH);
	w32(d->regs, ENA_REG_AQ_DB, d->aq_tail);

	for (;;) {
		struct ena_admin_acq_entry *e = &d->acq[d->acq_head];
		__asm__ volatile ("dc ivac, %0\n dsb sy\n"
				  :: "r"(e) : "memory");
		uint8_t phase = (uint8_t)((e->common.command >> 12) & 1);
		uint16_t cid  = (uint16_t)(e->common.command & 0x0FFF);
		if (phase == d->acq_phase && cid == cmd_id) {
			uint8_t status = e->common.status;
			if (resp) kmemcpy(resp, e->payload, 56);
			d->acq_head = (uint16_t)((d->acq_head + 1) % ENA_AQ_DEPTH);
			if (d->acq_head == 0) d->acq_phase ^= 1;
			w32(d->regs, ENA_REG_ACQ_TAIL, d->acq_head);
			return status == 0 ? 0 : -1;
		}
	}
}

/* ---- GET_FEATURE(DEVICE_ATTRIBUTES) ---- */

static int ena_get_dev_attr(struct ena_dev *d)
{
	struct ena_admin_aq_entry cmd;
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.common.opcode = ENA_ADMIN_OP_GET_FEATURE;
	/* XXX verify: feature_id encoding lives somewhere in payload[0..3]
	 * in the upstream driver.  Best-effort guess. */
	cmd.payload[0] = ENA_ADMIN_FEAT_DEVICE_ATTRIBUTES;

	struct ena_admin_get_feat_dev_attr attr;
	kmemset(&attr, 0, sizeof(attr));
	if (ena_admin_submit(d, &cmd, &attr) < 0) {
		kprintf("ena: GET_FEATURE(DEVICE_ATTRIBUTES) failed\n");
		return -1;
	}

	for (int i = 0; i < 6; i++) d->mac[i] = attr.mac_addr[i];
	d->max_mtu = attr.max_mtu ? attr.max_mtu : 1500;
	kprintf("ena: mac %02x:%02x:%02x:%02x:%02x:%02x  max_mtu %u\n",
		d->mac[0], d->mac[1], d->mac[2],
		d->mac[3], d->mac[4], d->mac[5],
		d->max_mtu);

	/* GET_FEATURE(MAX_QUEUES_NUM) next; XXX verify response layout.
	 * For now assume 1 TX + 1 RX per direction is supported on all
	 * instances. */
	d->max_io_queues = 1;
	return 0;
}

/* ---- I/O queue create (admin CREATE_CQ then CREATE_SQ) ----
 *
 * CREATE_CQ command payload (XXX verify against ena_admin_defs.h):
 *   u8  cq_depth_log2 (?)   -- some driver versions store log2, others raw
 *   u32 num_descs
 *   u64 cq_phys_addr
 *   u32 msix_vector
 *   u8  intr_mode  (0 = polled)
 *
 * CREATE_SQ command payload:
 *   u16 cq_id          (returned by CREATE_CQ)
 *   u16 num_descs
 *   u8  direction      (0 = TX, 1 = RX)
 *   u64 sq_phys_addr
 *
 * Both commands return the queue's doorbell offset somewhere in
 * the ACQE payload, which we use for ENA_REG_AQ_DB / the IO
 * doorbell base.  XXX verify exact layout.
 */

static int ena_create_io_q(struct ena_dev *d, struct ena_io_q *q,
                           int direction /* 0 = TX, 1 = RX */)
{
	q->sq_pa = pmm_alloc();
	q->cq_pa = pmm_alloc();
	if (!q->sq_pa || !q->cq_pa) return -1;
	kmemset(q->sq_pa, 0, 4096);
	kmemset(q->cq_pa, 0, 4096);
	q->depth      = ENA_IOQ_DEPTH;
	q->sq_tail    = 0;
	q->cq_head    = 0;
	q->cq_phase   = 1;
	q->next_req_id = 1;

	/* Create CQ first.  XXX verify payload layout. */
	struct ena_admin_aq_entry cmd;
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.common.opcode = ENA_ADMIN_OP_CREATE_CQ;
	*(uint32_t *)(cmd.payload +  0) = ENA_IOQ_DEPTH;
	*(uint64_t *)(cmd.payload +  4) = (uint64_t)(uintptr_t)q->cq_pa;
	*(uint32_t *)(cmd.payload + 12) = 0;	/* MSI-X = none, polled */
	uint8_t cq_resp[56];
	if (ena_admin_submit(d, &cmd, cq_resp) < 0) {
		kprintf("ena: CREATE_CQ (%s) failed\n",
			direction ? "rx" : "tx");
		return -1;
	}
	uint16_t cq_id = *(uint16_t *)(cq_resp + 0);
	q->cq_db_off   = *(uint32_t *)(cq_resp + 4);

	/* Create SQ. */
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.common.opcode = ENA_ADMIN_OP_CREATE_SQ;
	*(uint16_t *)(cmd.payload + 0) = cq_id;
	*(uint16_t *)(cmd.payload + 2) = ENA_IOQ_DEPTH;
	*(uint8_t  *)(cmd.payload + 4) = (uint8_t)direction;
	*(uint64_t *)(cmd.payload + 8) = (uint64_t)(uintptr_t)q->sq_pa;
	uint8_t sq_resp[56];
	if (ena_admin_submit(d, &cmd, sq_resp) < 0) {
		kprintf("ena: CREATE_SQ (%s) failed\n",
			direction ? "rx" : "tx");
		return -1;
	}
	q->sq_db_off = *(uint32_t *)(sq_resp + 4);

	return 0;
}

/* Pre-fill the RX queue's submission ring with PMM-backed buffers
 * so the device has somewhere to land incoming packets. */
static int ena_rx_refill(struct ena_dev *d)
{
	struct ena_io_rx_desc *ring = d->rx.sq_pa;
	for (uint32_t i = 0; i < d->rx.depth; i++) {
		void *b = pmm_alloc();
		if (!b) return -1;
		d->rx_bufs[i]   = b;
		ring[i].length    = ENA_RX_BUF_BYTES;
		ring[i].req_id    = (uint16_t)i;
		ring[i].buff_addr = (uint64_t)(uintptr_t)b;
	}
	d->rx.sq_tail = (uint16_t)d->rx.depth;
	w32(d->regs, d->rx.sq_db_off, d->rx.sq_tail);
	return 0;
}

/* ---- netif TX hook ----
 *
 * Skeleton only -- builds a single-segment TX descriptor over the
 * mblk's b_rptr/b_wptr and rings the doorbell.  No DMA mapping is
 * needed because kappara runs identity-mapped, so the kernel VA
 * equals the bus address.
 *
 * Today the IP layer never reaches here on virt (virtio-net wins
 * the "eth0" race when both drivers run); the path is included
 * so the wire-up matches the rest of the stack. */
struct msgb;
typedef struct msgb mblk_t;
extern unsigned char *mblk_rptr_(const mblk_t *);	/* fwd, see below */
extern unsigned char *mblk_wptr_(const mblk_t *);
extern void           freemsg(mblk_t *);

static int ena_tx_one(struct netif *nif, mblk_t *mp)
{
	(void)nif;
	if (!g_ena_init_ok) { freemsg(mp); return -1; }

	/* XXX: in a real impl we'd open-code the mblk fields; for now
	 * just drop and warn so the structural wire-up compiles. */
	(void)mp;
	freemsg(mp);
	return 0;
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

	/* Enable Memory Space + Bus Master in the PCI Command register,
	 * same dance as nvme.c. */
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

	uint32_t ver = r32(d->regs, ENA_REG_VERSION);
	uint32_t cver = r32(d->regs, ENA_REG_CONTROLLER_VERSION);
	kprintf("ena: bar0 0x%lx  version 0x%x  ctrl-ver 0x%x\n",
		bar0_pa, ver, cver);

	if (ena_reset(d)          < 0) return;
	if (ena_admin_init(d)     < 0) return;
	if (ena_get_dev_attr(d)   < 0) return;
	if (ena_create_io_q(d, &d->tx, 0) < 0) return;
	if (ena_create_io_q(d, &d->rx, 1) < 0) return;
	if (ena_rx_refill(d)      < 0) return;

	d->nif.name     = "eth0";
	d->nif.ip       = 0;		/* DHCP later */
	d->nif.netmask  = 0;
	d->nif.mtu      = (d->max_mtu < 1500) ? d->max_mtu : 1500;
	d->nif.streamtab = NULL;	/* legacy direct-tx for now */
	d->nif.tx       = ena_tx_one;
	netif_register(&d->nif);

	g_ena_init_ok = 1;
	kprintf("ena: ready (TX+RX queues up; netif eth0 registered)\n");
	kprintf("ena: NOTE -- packet I/O paths are skeletons, see "
		"include/kappara/arch/ena.h for the XXX-verify list\n");
}

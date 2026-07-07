/*
 * uts/virt/ena.c -- AWS ENA network driver
 *
 * Amazon Elastic Network Adapter.  PCI vendor 0x1d0f, device 0xec20 /
 * 0xec21.  Probed via pci_devs[], BAR0 mapped with mmu_map_device_1gb,
 * admin queue polled (no MSI-X).
 *
 * Register offsets and bit definitions verified against:
 *   torvalds/linux drivers/net/ethernet/amazon/ena/ena_regs_defs.h
 *   torvalds/linux drivers/net/ethernet/amazon/ena/ena_admin_defs.h
 *
 * Steps implemented:
 *   1. PCI probe: vendor 0x1d0f, device 0xec20/ec21, class 0x0200.
 *   2. Enable PCI Memory Space + Bus Master in Command register.
 *   3. Map BAR0 (registers) with mmu_map_device_1gb.
 *   4. Reset: set DEV_CTL.RESET, wait RESET_IN_PROGRESS, RESET_FINISHED,
 *      clear CTL, wait READY.
 *   5. Allocate Admin SQ + Admin CQ + AENQ pages from PMM.
 *   6. Program AQ/ACQ/AENQ base addresses and CAPS registers.
 *   7. GET_FEATURE(DEVICE_ATTRIBUTES) → MAC, max_mtu.
 *   8. CREATE_CQ + CREATE_SQ for one TX pair and one RX pair.
 *   9. Refill RX descriptors with PMM-backed buffers.
 *  10. Register a struct netif "eth0".
 *  11. (Future) RX kthread or interrupt-driven drain; TX path completes.
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
#define ENA_REG_AENQ_TAIL		0x44	/* init to 0 after reset */
#define ENA_REG_INTR_MASK		0x4C
#define ENA_REG_DEV_CTL			0x54
#define ENA_REG_DEV_STS			0x58
#define ENA_REG_MMIO_REG_READ		0x5C
#define ENA_REG_MMIO_RESP_LO		0x60
#define ENA_REG_MMIO_RESP_HI		0x64

/* CAPS register: reset timeout in bits[5:1] (units = 100 ms) */
#define ENA_CAPS_RESET_TIMEOUT_SHIFT	1
#define ENA_CAPS_RESET_TIMEOUT_MASK	0x3E

/* DEV_CTL bits (ena_regs_defs.h) */
#define ENA_DEV_CTL_RESET		(1u << 0)
#define ENA_DEV_CTL_AQ_RESTART		(1u << 1)
#define ENA_DEV_CTL_QUIESCENT		(1u << 2)
#define ENA_DEV_CTL_IO_RESUME		(1u << 3)
#define ENA_DEV_CTL_RESET_REASON_SHIFT	28	/* NORMAL=0 */

/* DEV_STS bits (ena_regs_defs.h) */
#define ENA_DEV_STS_READY		(1u << 0)	/* 0x01 */
#define ENA_DEV_STS_AQ_RESTART_PROG	(1u << 1)	/* 0x02 */
#define ENA_DEV_STS_AQ_RESTART_DONE	(1u << 2)	/* 0x04 */
#define ENA_DEV_STS_RESET_IN_PROGRESS	(1u << 3)	/* 0x08 */
#define ENA_DEV_STS_RESET_FINISHED	(1u << 4)	/* 0x10 */
#define ENA_DEV_STS_FATAL_ERROR		(1u << 5)	/* 0x20 */

/* AQ/ACQ/AENQ CAPS register layout:
 *   bits[15:0]  = depth (number of entries)
 *   bits[31:16] = entry size in bytes
 */
#define ENA_QCAPS(depth, entry_size) \
	(((uint32_t)(entry_size) << 16) | (uint32_t)(depth))

/* ---- Admin queue structures (ena_admin_defs.h) ----
 *
 * AQ entry: 64 bytes.
 *   common_desc:       4 bytes (command_id u16, opcode u8, flags u8)
 *   command-specific: 60 bytes
 *
 * ACQ entry: 64 bytes.
 *   common_desc:       8 bytes
 *   response payload: 56 bytes
 *
 * AENQ entry: 64 bytes.
 *
 * Phase bit: carried in common_desc.flags bit 0 for SQ entries, in
 * acq_common_desc.flags bit 0 for CQ entries.  Toggles on ring wrap.
 */

/* SQ common descriptor (4 bytes) */
struct ena_admin_aq_common_desc {
	uint16_t command_id;	/* bits[11:0] = id; bits[15:12] = reserved */
	uint8_t  opcode;
	uint8_t  flags;		/* bit[0] = phase, bit[1] = ctrl_data,
				 * bit[2] = ctrl_data_indirect */
} __attribute__((packed));

/* CQ common descriptor (8 bytes) */
struct ena_admin_acq_common_desc {
	uint16_t command;	/* bits[11:0] = command_id echoed */
	uint8_t  status;	/* 0 = success */
	uint8_t  flags;		/* bit[0] = phase tag */
	uint16_t extended_status;
	uint16_t sq_head_indx;
} __attribute__((packed));

/* AQ entry: 4-byte header + 60-byte payload = 64 bytes */
struct ena_admin_aq_entry {
	struct ena_admin_aq_common_desc common;	/* 4 bytes */
	uint8_t                         payload[60];
} __attribute__((packed));

/* CQ entry: 8-byte header + 56-byte payload = 64 bytes */
struct ena_admin_acq_entry {
	struct ena_admin_acq_common_desc common;	/* 8 bytes */
	uint8_t                          payload[56];
} __attribute__((packed));

/* AENQ entry: 64 bytes */
struct ena_aenq_entry {
	uint16_t group;
	uint16_t syndrome;
	uint8_t  flags;		/* bit[0] = phase tag */
	uint8_t  reserved[3];
	uint64_t timestamp_us;
	uint8_t  payload[48];	/* 2+2+1+3+8+48 = 64 */
} __attribute__((packed));

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
#define ENA_ADMIN_FEAT_MAX_QUEUES_NUM		2
#define ENA_ADMIN_FEAT_HW_HINTS			3
#define ENA_ADMIN_FEAT_LLQ			4
#define ENA_ADMIN_FEAT_MTU			14
#define ENA_ADMIN_FEAT_HOST_ATTR_CONFIG		16

/* Control-buffer descriptor: 12 bytes
 *   u32 length
 *   u32 mem_addr_lo
 *   u16 mem_addr_hi
 *   u16 reserved
 * Used in inline GET/SET_FEATURE commands when ctrl_data=0 (no buffer). */
struct ena_admin_ctrl_buff {
	uint32_t length;
	uint32_t mem_addr_lo;
	uint16_t mem_addr_hi;
	uint16_t reserved;
} __attribute__((packed));

/* GET_FEATURE / SET_FEATURE feature selector: 4 bytes
 *   u8  flags        (bit[0] = relative_feat_id, bit[1] = vf_exist)
 *   u8  feature_id   (ENA_ADMIN_FEAT_*)
 *   u16 feature_version
 */
struct ena_admin_feat_common {
	uint8_t  flags;
	uint8_t  feature_id;
	uint16_t feature_version;
} __attribute__((packed));

/* GET_FEATURE(DEVICE_ATTRIBUTES) response payload layout (56 bytes max in
 * ACQ payload).  Starts at payload[0] of the ACQ entry. */
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

/* MMIO register read-less response buffer (8 bytes, one entry).
 * The device DMA-writes the response here when we trigger an indirect
 * read via ENA_REG_MMIO_REG_READ.  Layout from ena_admin_defs.h:
 *   u16 req_id     -- echoes the seq number we sent
 *   u16 reg_off    -- echoes the register offset
 *   u32 reg_val    -- the register value
 * See ena_com.c::ena_com_mmio_reg_read_request_init. */
struct ena_mmio_read_resp {
	uint16_t req_id;
	uint16_t reg_off;
	uint32_t reg_val;
} __attribute__((packed));

/* ---- I/O queue descriptors ----
 *
 * TX SQ descriptor: 16 bytes.
 * RX SQ descriptor: 16 bytes.
 * Completion (CQ) descriptor: 16 bytes, with phase bit in flags.
 */

#define ENA_TX_CTRL_FIRST	(1u << 7)
#define ENA_TX_CTRL_LAST	(1u << 6)
#define ENA_TX_CTRL_COMP_REQ	(1u << 5)

struct ena_io_tx_desc {
	uint16_t length;
	uint8_t  reserved;
	uint8_t  ctrl;		/* FIRST | LAST | COMP_REQ */
	uint16_t req_id;
	uint8_t  reserved2[2];
	uint64_t buff_addr;
} __attribute__((packed));

struct ena_io_rx_desc {
	uint16_t length;	/* buffer capacity */
	uint16_t req_id;
	uint8_t  reserved[4];
	uint64_t buff_addr;
} __attribute__((packed));

struct ena_io_cdesc {
	uint16_t req_id;
	uint8_t  status;
	uint8_t  flags;		/* bit[0] = phase tag */
	uint16_t length;	/* RX: actual bytes received */
	uint8_t  reserved[10];
} __attribute__((packed));

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
	uint16_t next_req_id;
};

struct ena_dev {
	volatile uint8_t *regs;

	/* MMIO read-less mechanism: device DMA-writes register values here.
	 * Allocated in ena_mmio_read_init(); NULL until then (callers fall
	 * back to direct r32 while it's unset). */
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
	uint32_t max_io_queues;

	struct ena_io_q tx;
	struct ena_io_q rx;

	void    *rx_bufs[ENA_IOQ_DEPTH];

	struct netif nif;
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

/* ---- MMIO read-less: indirect register read ----
 *
 * The ENA spec (ena_regs_defs.h / ena_com.c) warns that direct BAR
 * reads of control registers may return stale values on some Nitro
 * firmware versions.  The preferred path is:
 *   1. Host writes the response-buffer PA to MMIO_RESP_LO/HI once.
 *   2. For each read: write (reg_off<<16 | seq) to MMIO_REG_READ.
 *   3. Device DMA-writes an 8-byte record (req_id, reg_off, reg_val)
 *      into the response buffer; host polls req_id for the match.
 * Falls back to direct r32 when mmio_resp is unset (early init) or
 * if the device times out (1 ms). */
#define ENA_RESET_CAPS_MAX_UNITS	40	/* 4 s hard cap */

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

	/* Invalidate the response buffer so we see the device's DMA write. */
	__asm__ volatile ("dc ivac, %0\n\t" "dsb sy\n\t"
			  :: "r"(d->mmio_resp) : "memory");

	/* Trigger: bits[15:0] = req_id, bits[31:16] = reg_off. */
	w32(d->regs, ENA_REG_MMIO_REG_READ,
	    (uint32_t)seq | ((uint32_t)off << 16));

	/* Poll for completion (1 ms using the AArch64 generic counter). */
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
	return r32(d->regs, off);	/* timeout fallback */
}

/* ---- Reset ----
 *
 * Linux driver sequence (ena_com.c ena_com_dev_reset):
 *   1. Check DEV_STS.READY -- must be set before we can reset.
 *   2. Read CAPS bits[5:1] to get the device-advertised reset timeout
 *      (each unit = 100 ms).
 *   3. Write DEV_CTL = RESET | (NORMAL_REASON << 28).  Write twice;
 *      the second write is required by the spec.
 *   4. Poll DEV_STS.RESET_IN_PROGRESS (bit 3).
 *   5. Poll DEV_STS.RESET_FINISHED (bit 4) -- takes up to
 *      caps_timeout × 100 ms.
 *   6. Clear DEV_CTL.
 *   7. Poll DEV_STS.READY (bit 0).
 *
 * Timing: uses CNTPCT_EL0 + CNTFRQ_EL0 for wall-clock correctness.
 * Spin-count estimation was unreliable because MMIO read latency on
 * Nitro varies; real-time polling removes the uncertainty entirely.
 *
 * Prior-reset handling: when the previous OS boot left a reset in
 * progress (STS has RESET_IN_PROGRESS set at entry), we wait for it
 * to finish before triggering a new one.  Stacking resets confuses
 * the ENA firmware and guarantees RESET_FINISHED never appears. */

/* Poll DEV_STS (via indirect read) until (sts & mask)==expected.
 * Returns the matching sts value on success, -1 on FATAL_ERROR,
 * -2 on timeout. */
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

	/* If a prior boot left RESET_IN_PROGRESS, the previous OS wrote
	 * DEV_CTL=RESET and then crashed before clearing it.  The device is
	 * waiting for DEV_CTL to be cleared before it can finish.  Clear
	 * DEV_CTL first, then wait for RESET_IN_PROGRESS to go away. */
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

	/* Write reset command once.  Immediately re-write the MMIO response
	 * buffer address: the device loses its DMA config when it sees the
	 * reset bit and will not respond to indirect reads until we re-arm it.
	 * (Linux: writel(reset_val); ena_com_mmio_reg_read_request_write_dev_addr) */
	w32(d->regs, ENA_REG_DEV_CTL, ENA_DEV_CTL_RESET);
	if (d->mmio_resp) {
		uint64_t pa = (uint64_t)(uintptr_t)d->mmio_resp;
		w32(d->regs, ENA_REG_MMIO_RESP_LO, (uint32_t)pa);
		w32(d->regs, ENA_REG_MMIO_RESP_HI, (uint32_t)(pa >> 32));
	}

	/* Step 1: wait for RESET_IN_PROGRESS to set (device acks). */
	{
		uint64_t freq, start, deadline;
		__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
		__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
		deadline = start + (freq / 1000ULL) * 1000ULL;   /* 1s */
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

	/* Step 2: clear DEV_CTL first, THEN wait for RESET_IN_PROGRESS to
	 * clear.  Linux: writel(0, DEV_CTL); wait_for_reset_state(0).
	 * The device will not complete reset until we release the reset bit. */
	w32(d->regs, ENA_REG_DEV_CTL, 0);

	int rc = wait_dev_sts_ms(d, ENA_DEV_STS_RESET_IN_PROGRESS,
				 0, timeout_ms);
	if (rc < 0) {
		kprintf("ena: reset timed out (RESET_IN_PROGRESS stuck, "
			"STS=0x%x after %ums)\n",
			ena_r32(d, ENA_REG_DEV_STS), timeout_ms);
		return -1;
	}

	rc = wait_dev_sts_ms(d, ENA_DEV_STS_READY, ENA_DEV_STS_READY, timeout_ms);
	if (rc < 0) {
		kprintf("ena: timed out waiting for READY after reset (STS=0x%x)\n",
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
	d->acq_phase   = 1;	/* expected phase from device starts at 1 */
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

	/* Mask all interrupts; driver polls. */
	w32(d->regs, ENA_REG_INTR_MASK, 0xFFFFFFFFu);
	return 0;
}

/* Submit one admin command, poll the ACQ for the matching CQE.
 * Copies the 56-byte ACQ response payload into `resp` if non-NULL.
 * Returns 0 on status==0, -1 otherwise.
 *
 * Phase protocol:
 *   SQ: we toggle aq_phase on every depth wrap and set it in flags[0].
 *   CQ: device toggles phase on every depth wrap; we compare flags[0].
 */
/* Returns 0 on success, device status code (>0) on device error,
 * -1 on admin queue timeout. */
static int ena_admin_submit(struct ena_dev *d,
			    struct ena_admin_aq_entry *cmd,
			    void *resp)
{
	uint16_t cmd_id = d->next_cmd_id++;

	struct ena_admin_aq_entry *slot = &d->aq[d->aq_tail];
	*slot = *cmd;
	slot->common.command_id = cmd_id;
	/* Set phase bit in SQ entry flags. */
	slot->common.flags = (uint8_t)((slot->common.flags & ~1u) | d->aq_phase);

	__asm__ volatile ("dsb sy" ::: "memory");

	d->aq_tail++;
	if (d->aq_tail >= ENA_AQ_DEPTH) {
		d->aq_tail = 0;
		d->aq_phase ^= 1;
	}
	w32(d->regs, ENA_REG_AQ_DB, d->aq_tail);

	/* Poll CQ for the matching response.  Phase bit is in flags[0] of
	 * the ACQ common descriptor, NOT in command[12].
	 * Hard timeout: 5 s via CNTPCT_EL0 to avoid infinite spin on a
	 * broken admin queue. */
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
			kprintf("ena: admin cmd %u timeout (opcode=%u, "
				"acq_head=%u phase=%u cid=%u)\n",
				cmd_id, cmd->common.opcode,
				d->acq_head, phase, cid);
			return -1;
		}
	}
}

/* ---- GET_FEATURE(DEVICE_ATTRIBUTES) ---- */

static int ena_get_dev_attr(struct ena_dev *d)
{
	struct ena_admin_aq_entry cmd;
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.common.opcode = ENA_ADMIN_OP_GET_FEATURE;

	/* GET_FEATURE layout (offsets within the 60-byte payload):
	 *   [0..11]  ctrl_buff (length=0, addr=0 for inline request)
	 *   [12..15] feat_common: flags(1), feature_id(1), version(2)
	 * feature_id is at payload[13]. */
	struct ena_admin_feat_common *fc =
		(struct ena_admin_feat_common *)(cmd.payload + 12);
	fc->feature_id = ENA_ADMIN_FEAT_DEVICE_ATTRIBUTES;

	/* Response payload is 56 bytes; struct is 36 -- use a full-size buffer. */
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
	d->max_io_queues = 1;
	return 0;
}

/* ---- I/O queue creation (CREATE_CQ then CREATE_SQ) ----
 *
 * Struct layouts verified against torvalds/linux
 * drivers/net/ethernet/amazon/ena/ena_admin_defs.h.
 *
 * CREATE_CQ payload (ena_admin_aq_create_cq_cmd, after 4-byte common):
 *   byte  0     cq_caps_1: bit5=interrupt_mode_enabled (0=polling)
 *   byte  1     cq_caps_2: bits[4:0]=entry_size_words (4=16-byte entries)
 *   bytes 2-3   cq_depth (u16, power of 2)
 *   bytes 4-7   msix_vector (u32, 0=none)
 *   bytes 8-11  cq_ba.mem_addr_low (u32)
 *   bytes 12-13 cq_ba.mem_addr_high (u16, upper 16 bits of 48-bit PA)
 *   bytes 14-15 cq_ba.reserved16 = 0
 *
 * CREATE_CQ response (ena_admin_acq_create_cq_resp_desc, after 8-byte common):
 *   bytes 0-1   cq_idx (u16)
 *   bytes 2-3   cq_actual_depth (u16)
 *   bytes 4-7   numa_node_register_offset (u32)
 *   bytes 8-11  cq_head_db_register_offset (u32)  <-- this is the doorbell
 *   bytes 12-15 cq_interrupt_unmask_register_offset (u32)
 *
 * CREATE_SQ payload (ena_admin_aq_create_sq_cmd, after 4-byte common):
 *   byte  0     sq_identity: bits[7:5]=sq_direction (0x1=TX, 0x2=RX)
 *   byte  1     reserved
 *   byte  2     sq_caps_2: bits[3:0]=placement_policy (0=host mem)
 *   byte  3     sq_caps_3: bit[0]=is_physically_contiguous (1)
 *   bytes 4-5   cq_idx (u16)
 *   bytes 6-7   sq_depth (u16)
 *   bytes 8-11  sq_ba.mem_addr_low (u32)
 *   bytes 12-13 sq_ba.mem_addr_high (u16)
 *   bytes 14-15 sq_ba.reserved16 = 0
 *   bytes 16-23 sq_head_writeback (0 = no writeback)
 *
 * CREATE_SQ response (ena_admin_acq_create_sq_resp_desc, after 8-byte common):
 *   bytes 0-1   sq_idx (u16)
 *   bytes 2-3   reserved (u16)
 *   bytes 4-7   sq_doorbell_offset (u32)
 */
static int ena_create_io_q(struct ena_dev *d, struct ena_io_q *q,
			   int direction)
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
	q->sq_phase   = 0;
	q->next_req_id = 1;

	struct ena_admin_aq_entry cmd;
	uint8_t resp[56];

	/* CREATE_CQ */
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.common.opcode = ENA_ADMIN_OP_CREATE_CQ;
	uint64_t cq_pa = (uint64_t)(uintptr_t)q->cq_pa;
	cmd.payload[0] = 0;				/* cq_caps_1: polling */
	cmd.payload[1] = 4;				/* cq_caps_2: 4 words = 16 B */
	*(uint16_t *)(cmd.payload + 2) = (uint16_t)ENA_IOQ_DEPTH;
	*(uint32_t *)(cmd.payload + 4) = 0;		/* msix_vector */
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
	q->cq_db_off   = *(uint32_t *)(resp + 8);	/* cq_head_db_register_offset */

	/* CREATE_SQ */
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.common.opcode = ENA_ADMIN_OP_CREATE_SQ;
	uint64_t sq_pa = (uint64_t)(uintptr_t)q->sq_pa;
	/* sq_identity bits[7:5]=direction; 0x1=TX, 0x2=RX */
	cmd.payload[0] = (uint8_t)((direction + 1) << 5);
	cmd.payload[1] = 0;			/* reserved */
	/* sq_caps_2 bits[3:0]=placement_policy: 1=host memory */
	cmd.payload[2] = 0x01;
	/* sq_caps_3 bit[0]=phase_desc, bit[1]=is_physically_contiguous.
	 * We want is_physically_contiguous=1 → bit 1 → value 0x02. */
	cmd.payload[3] = 0x02;
	*(uint16_t *)(cmd.payload + 4) = cq_id;
	*(uint16_t *)(cmd.payload + 6) = (uint16_t)ENA_IOQ_DEPTH;
	*(uint32_t *)(cmd.payload + 8) = (uint32_t)sq_pa;
	*(uint16_t *)(cmd.payload + 12) = (uint16_t)(sq_pa >> 32);
	/* bytes 14-23: reserved + sq_head_writeback = 0 */
	kmemset(resp, 0, sizeof(resp));
	{
		int rc = ena_admin_submit(d, &cmd, resp);
		if (rc != 0) {
			kprintf("ena: CREATE_SQ (%s) failed (status=%d)\n",
				direction ? "rx" : "tx", rc);
			return -1;
		}
	}
	q->sq_db_off = *(uint32_t *)(resp + 4);	/* sq_doorbell_offset */

	return 0;
}

/* Pre-fill the RX submission ring with PMM-backed buffers. */
static int ena_rx_refill(struct ena_dev *d)
{
	struct ena_io_rx_desc *ring = d->rx.sq_pa;
	for (uint32_t i = 0; i < d->rx.depth; i++) {
		void *b = pmm_alloc();
		if (!b) return -1;
		d->rx_bufs[i]     = b;
		ring[i].length    = ENA_RX_BUF_BYTES;
		ring[i].req_id    = (uint16_t)i;
		ring[i].buff_addr = (uint64_t)(uintptr_t)b;
	}
	d->rx.sq_tail = (uint16_t)d->rx.depth;
	w32(d->regs, d->rx.sq_db_off, d->rx.sq_tail);
	return 0;
}

/* ---- netif TX hook (skeleton) ---- */
struct msgb;
typedef struct msgb mblk_t;
extern void freemsg(mblk_t *);

static int ena_tx_one(struct netif *nif, mblk_t *mp)
{
	(void)nif;
	if (!g_ena_init_ok) { freemsg(mp); return -1; }
	/* TODO: build TX descriptor, ring doorbell, poll completion */
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

	/* Set up the MMIO read-less mechanism before any register reads so
	 * that control-register reads (CAPS, DEV_STS) go through the DMA
	 * path and are not affected by any direct-read stale-value quirks
	 * in the Nitro ENA firmware. */
	ena_mmio_read_init(d);

	uint32_t ver  = ena_r32(d, ENA_REG_VERSION);
	uint32_t cver = ena_r32(d, ENA_REG_CONTROLLER_VERSION);
	uint32_t sts  = ena_r32(d, ENA_REG_DEV_STS);
	kprintf("ena: bar0 0x%lx  ver 0x%x  ctrl-ver 0x%x  sts 0x%x\n",
		bar0_pa, ver, cver, sts);

	if (ena_reset(d)               < 0) return;
	if (ena_admin_init(d)          < 0) return;
	if (ena_get_dev_attr(d)        < 0) return;
	if (ena_create_io_q(d, &d->tx, 0) < 0) return;
	if (ena_create_io_q(d, &d->rx, 1) < 0) return;
	if (ena_rx_refill(d)           < 0) return;

	d->nif.name     = "eth0";
	d->nif.ip       = 0;
	d->nif.netmask  = 0;
	d->nif.mtu      = (d->max_mtu < 1500) ? d->max_mtu : 1500;
	d->nif.streamtab = NULL;
	d->nif.tx       = ena_tx_one;
	netif_register(&d->nif);

	g_ena_init_ok = 1;
	kprintf("ena: ready  mac %02x:%02x:%02x:%02x:%02x:%02x"
		"  mtu %u  eth0 registered\n",
		d->mac[0], d->mac[1], d->mac[2],
		d->mac[3], d->mac[4], d->mac[5],
		d->nif.mtu);
}

/* Returns 1 if an ENA NIC is present in the PCIe device list.
 * Used by kmain to suppress virtio_net_init on AWS Graviton. */
int ena_present(void)
{
	return ena_find() != NULL;
}

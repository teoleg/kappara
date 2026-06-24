/*
 * uts/virt/nvme.c -- AWS.md stage F: NVMe block driver
 *
 * Implements the minimal subset of NVMe 1.4 needed to read and
 * write one 512-byte LBA at a time -- exactly the surface area
 * struct block_device exposes upward to kfs.  Polled completion,
 * no MSI-X, one I/O queue pair.
 *
 * Bring-up sequence (NVMe spec §3.5.1):
 *   1. Read CAP, derive doorbell stride.
 *   2. CC.EN = 0; spin until CSTS.RDY = 0.
 *   3. Allocate one PMM page each for Admin SQ and Admin CQ; zero them.
 *   4. Program AQA (queue depths), ASQ, ACQ.
 *   5. CC = (IOSQES=6, IOCQES=4, AMS=0, MPS=0, CSS=0, EN=1).
 *   6. Spin until CSTS.RDY = 1.
 *   7. Identify Controller (CNS=1).
 *   8. Identify Namespace (CNS=0, NSID=1).
 *   9. Create I/O CQ (admin opc 0x05), Create I/O SQ (admin opc 0x01).
 *  10. Register a struct block_device whose bd_read/bd_write submit
 *      NVM Read (0x02) / Write (0x01) and poll.
 *
 * QEMU testing:  -device nvme,drive=hd,serial=foo -drive id=hd,...
 * On the wire the device is "Intel" 8086:5845 in older QEMU and
 * "Red Hat" 1b36:0010 in newer; both report PCI class 0x010802
 * (Mass Storage / NVM controller / NVMe), which is what we probe
 * on -- not the vendor ID.
 *
 * Limitations on purpose (Stage F scope):
 *   - PRP1 only.  bd_read/write transfer 512 B, well under 4 KB, so
 *     the buffer never straddles a page boundary -> PRP2 unused.
 *   - LBA size is read from the namespace but the block_device
 *     adapter only exposes 512 B.  If the namespace is 4 KB
 *     formatted we'd need to refuse or translate; for now we
 *     assert lba_bytes == 512.
 *   - Single namespace (NSID=1).  Multi-namespace controllers
 *     report N namespaces; we just take the first.
 *   - One I/O queue pair, polled.  No MSI-X, no service procs.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/arch/mmu.h"
#include "kappara/arch/nvme.h"
#include "kappara/arch/pcie.h"
#include "kappara/core/kmem.h"
#include "kappara/core/pmm.h"
#include "kappara/core/printk.h"
#include "kappara/core/string.h"
#include "kappara/fs/blkdev.h"

/* ---- Register offsets (NVMe 1.4 §3.1) ---- */
#define NVME_REG_CAP	0x00	/* 64-bit Capabilities */
#define NVME_REG_VS	0x08
#define NVME_REG_INTMS	0x0C
#define NVME_REG_INTMC	0x10
#define NVME_REG_CC	0x14
#define NVME_REG_CSTS	0x1C
#define NVME_REG_AQA	0x24
#define NVME_REG_ASQ	0x28
#define NVME_REG_ACQ	0x30

#define NVME_CC_EN	(1u << 0)
#define NVME_CSTS_RDY	(1u << 0)
#define NVME_CSTS_CFS	(1u << 1)	/* Controller Fatal Status */

#define NVME_AQ_DEPTH	32		/* fits in one 4 KB page (32*64 B SQ) */
#define NVME_IOQ_DEPTH	32

/* SQE / CQE -- exactly as in the spec. */
struct nvme_sqe {
	uint8_t  opc;
	uint8_t  fuse_psdt;
	uint16_t cid;
	uint32_t nsid;
	uint64_t reserved;
	uint64_t mptr;
	uint64_t prp1;
	uint64_t prp2;
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
} __attribute__((packed));

struct nvme_cqe {
	uint32_t cdw0;
	uint32_t reserved;
	uint16_t sqhd;
	uint16_t sqid;
	uint16_t cid;
	uint16_t status;	/* bit 0 = Phase Tag P, bits [15:1] = SC/SCT */
} __attribute__((packed));

/* Identify Controller (4 KB) -- only the bits we read. */
struct nvme_id_ctrl {
	uint16_t vid;
	uint16_t ssvid;
	char     sn[20];
	char     mn[40];
	char     fr[8];
	uint8_t  rab;
	uint8_t  ieee[3];
	uint8_t  cmic;
	uint8_t  mdts;
	/* ... lots more we ignore */
};

/* Identify Namespace (4 KB) -- enough to recover size + LBA format. */
struct nvme_id_ns {
	uint64_t nsze;	/* namespace size in LBAs */
	uint64_t ncap;
	uint64_t nuse;
	uint8_t  nsfeat;
	uint8_t  nlbaf;
	uint8_t  flbas;	/* low 4 bits select LBAF[i] */
	uint8_t  mc;
	uint8_t  dpc;
	uint8_t  dps;
	uint8_t  nmic;
	uint8_t  rescap;
	uint8_t  fpi;
	uint8_t  dlfeat;
	uint16_t nawun;
	uint16_t nawupf;
	uint16_t nacwu;
	uint16_t nabsn;
	uint16_t nabo;
	uint16_t nabspf;
	uint16_t noiob;
	uint8_t  nvmcap[16];
	uint8_t  reserved2[40];
	uint8_t  nguid[16];
	uint8_t  eui64[8];
	struct {
		uint16_t ms;
		uint8_t  lbads;	/* log2 of LBA data size */
		uint8_t  rp;
	} lbaf[16];
};

/* ---- Driver state ---- */

struct nvme_dev {
	volatile uint8_t *regs;		/* MMIO base (BAR0 mapped) */
	uint32_t          dstrd_shift;	/* 2 + CAP.DSTRD */

	struct nvme_sqe  *asq;
	struct nvme_cqe  *acq;
	uint16_t          asq_tail;
	uint16_t          acq_head;
	uint8_t           acq_phase;	/* expected Phase Tag (toggles every wrap) */

	struct nvme_sqe  *iosq;
	struct nvme_cqe  *iocq;
	uint16_t          iosq_tail;
	uint16_t          iocq_head;
	uint8_t           iocq_phase;

	uint16_t          next_cid;

	uint64_t          nsze;		/* LBAs in namespace 1 */
	uint32_t          lba_bytes;
	uint32_t          nsid;

	struct block_device bd;
};

static struct nvme_dev  nvme_singleton;
static int              nvme_singleton_init;
static struct nvme_info nvme_singleton_info;

/* ---- MMIO helpers (volatile loads/stores; the compiler can't
 * reorder these past each other in a single TU). */

static uint32_t mmio32(volatile uint8_t *base, unsigned off)
{
	return *(volatile uint32_t *)(base + off);
}
static void mmio32_w(volatile uint8_t *base, unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}
static uint64_t mmio64(volatile uint8_t *base, unsigned off)
{
	/* Some platforms forbid 64-bit MMIO loads to BAR-mapped regions;
	 * read as two 32-bit halves to be safe. */
	uint64_t lo = mmio32(base, off);
	uint64_t hi = mmio32(base, off + 4);
	return lo | (hi << 32);
}
static void mmio64_w(volatile uint8_t *base, unsigned off, uint64_t v)
{
	mmio32_w(base, off,     (uint32_t)v);
	mmio32_w(base, off + 4, (uint32_t)(v >> 32));
}

/* Doorbell addresses.  Stride between consecutive doorbells = 4 <<
 * DSTRD; doorbells start at offset 0x1000.  qid=0 is the admin
 * queue. */
static unsigned sq_db_off(struct nvme_dev *d, unsigned qid)
{
	return 0x1000 + (qid * 2)     * (1u << d->dstrd_shift);
}
static unsigned cq_db_off(struct nvme_dev *d, unsigned qid)
{
	return 0x1000 + (qid * 2 + 1) * (1u << d->dstrd_shift);
}

/* ---- Polled admin-command submitter ---- */

static int nvme_admin_submit_and_wait(struct nvme_dev *d,
                                      struct nvme_sqe *cmd)
{
	cmd->cid = d->next_cid++;

	struct nvme_sqe *slot = &d->asq[d->asq_tail];
	*slot = *cmd;

	/* Make the SQE write visible to the device before we ring
	 * the doorbell.  Without this barrier the doorbell MMIO can
	 * race ahead of the cached SQE store and the controller
	 * fetches stale memory. */
	__asm__ volatile ("dsb sy" ::: "memory");

	d->asq_tail = (uint16_t)((d->asq_tail + 1) % NVME_AQ_DEPTH);
	mmio32_w(d->regs, sq_db_off(d, 0), d->asq_tail);

	/* Spin on the completion queue's phase tag.  The Phase Tag bit
	 * toggles every queue wrap; we entered with phase=1, so the
	 * first valid CQE has bit 0 = 1.  When we wrap acq_head past
	 * NVME_AQ_DEPTH, flip the expected phase.
	 *
	 * The DC IVAC inside the loop is mandatory on ARM: the queue
	 * memory is Normal cacheable, and PCIe DMA from the controller
	 * doesn't snoop our caches.  Without the invalidate we'd keep
	 * reading whatever the L1 captured at queue-init time (zeros). */
	for (;;) {
		struct nvme_cqe *cqe = &d->acq[d->acq_head];
		__asm__ volatile ("dc ivac, %0\n dsb sy\n"
				  :: "r"(cqe) : "memory");
		uint16_t s = cqe->status;
		if ((s & 1) == d->acq_phase) {
			uint16_t sc_sct = (uint16_t)(s >> 1) & 0x3FF;
			/* Advance head + doorbell. */
			d->acq_head = (uint16_t)((d->acq_head + 1) % NVME_AQ_DEPTH);
			if (d->acq_head == 0) d->acq_phase ^= 1;
			mmio32_w(d->regs, cq_db_off(d, 0), d->acq_head);
			return sc_sct ? -1 : 0;
		}
	}
}

/* ---- Polled I/O-command submitter ---- */

static int nvme_io_submit_and_wait(struct nvme_dev *d,
                                   struct nvme_sqe *cmd)
{
	cmd->cid = d->next_cid++;

	struct nvme_sqe *slot = &d->iosq[d->iosq_tail];
	*slot = *cmd;

	__asm__ volatile ("dsb sy" ::: "memory");

	d->iosq_tail = (uint16_t)((d->iosq_tail + 1) % NVME_IOQ_DEPTH);
	mmio32_w(d->regs, sq_db_off(d, 1), d->iosq_tail);

	for (;;) {
		struct nvme_cqe *cqe = &d->iocq[d->iocq_head];
		__asm__ volatile ("dc ivac, %0\n dsb sy\n"
				  :: "r"(cqe) : "memory");
		uint16_t s = cqe->status;
		if ((s & 1) == d->iocq_phase) {
			uint16_t sc_sct = (uint16_t)(s >> 1) & 0x3FF;
			d->iocq_head = (uint16_t)((d->iocq_head + 1) % NVME_IOQ_DEPTH);
			if (d->iocq_head == 0) d->iocq_phase ^= 1;
			mmio32_w(d->regs, cq_db_off(d, 1), d->iocq_head);
			return sc_sct ? -1 : 0;
		}
	}
}

/* ---- block_device adapters ---- */

static int nvme_bd_read(struct block_device *bd, uint32_t blkno, void *buf)
{
	struct nvme_dev *d = &nvme_singleton;
	if (blkno >= bd->bd_nblocks) return -1;

	struct nvme_sqe cmd = { 0 };
	cmd.opc   = 0x02;			/* NVM Read */
	cmd.nsid  = d->nsid;
	cmd.prp1  = (uint64_t)(uintptr_t)buf;
	cmd.cdw10 = (uint32_t)blkno;
	cmd.cdw11 = (uint32_t)((uint64_t)blkno >> 32);
	cmd.cdw12 = 0;				/* NLB = 0 (=> 1 LBA) */
	return nvme_io_submit_and_wait(d, &cmd);
}

static int nvme_bd_write(struct block_device *bd,
                         uint32_t blkno, const void *buf)
{
	struct nvme_dev *d = &nvme_singleton;
	if (blkno >= bd->bd_nblocks) return -1;

	struct nvme_sqe cmd = { 0 };
	cmd.opc   = 0x01;			/* NVM Write */
	cmd.nsid  = d->nsid;
	cmd.prp1  = (uint64_t)(uintptr_t)buf;
	cmd.cdw10 = (uint32_t)blkno;
	cmd.cdw11 = (uint32_t)((uint64_t)blkno >> 32);
	cmd.cdw12 = 0;
	return nvme_io_submit_and_wait(d, &cmd);
}

/* ---- Controller bring-up ---- */

static int nvme_setup(struct nvme_dev *d, struct pci_device *p)
{
	uint64_t bar0_pa = pcie_bar_addr(p, 0);
	if (!bar0_pa) {
		kprintf("nvme: BAR0 not a memory BAR\n");
		return -1;
	}

	/* Make sure the controller decodes its BAR.  UEFI's PCI bus
	 * driver leaves Memory Space + Bus Master disabled on devices
	 * it didn't drive itself (the boot disk gets enabled; an
	 * unrelated NVMe controller does not).  Without these bits the
	 * BAR window pulls all-ones for both reads and writes -- looks
	 * exactly like "no device", which is exactly what we got the
	 * first time around.  Cmd register lives at offset 0x04 of
	 * config space; PCIe ECAM exposes it identically to legacy PCI. */
	volatile uint16_t *cfg_cmd = (volatile uint16_t *)
		(uintptr_t)(pcie_ecam_base + ((uint64_t)p->bus << 20) +
		            ((uint64_t)p->dev << 15) +
		            ((uint64_t)p->fn  << 12) + 0x04);
	*cfg_cmd = (uint16_t)(*cfg_cmd | 0x0006);	/* MSE | BME */

	/* Map the BAR's 1 GB block as Device-nGnRE.  NVMe registers
	 * + doorbells fit in <16 KB, but our coarsest mapping unit is
	 * 1 GB; on QEMU virt highmem this lands at 0x80...000. */
	mmu_map_device_1gb(bar0_pa);
	d->regs = (volatile uint8_t *)(uintptr_t)bar0_pa;

	uint64_t cap = mmio64(d->regs, NVME_REG_CAP);
	if (cap == (uint64_t)-1 || cap == 0) {
		kprintf("nvme: CAP read failed (got 0x%lx)\n", cap);
		return -1;
	}
	unsigned dstrd = (unsigned)((cap >> 32) & 0xf);
	d->dstrd_shift = 2 + dstrd;

	uint32_t vs = mmio32(d->regs, NVME_REG_VS);
	nvme_singleton_info.bar0     = bar0_pa;
	nvme_singleton_info.major    = (vs >> 16) & 0xffff;
	nvme_singleton_info.minor    = (vs >> 8)  & 0xff;
	nvme_singleton_info.tertiary = vs & 0xff;
	kprintf("nvme: bar0 0x%lx  version %u.%u.%u  dstrd %u  mqes %u\n",
		bar0_pa,
		(vs >> 16) & 0xffff, (vs >> 8) & 0xff, vs & 0xff,
		dstrd, (unsigned)((cap & 0xffff) + 1));

	/* Disable controller, wait for CSTS.RDY = 0. */
	uint32_t cc = mmio32(d->regs, NVME_REG_CC);
	cc &= ~NVME_CC_EN;
	mmio32_w(d->regs, NVME_REG_CC, cc);
	for (;;) {
		uint32_t csts = mmio32(d->regs, NVME_REG_CSTS);
		if (!(csts & NVME_CSTS_RDY)) break;
		if (csts & NVME_CSTS_CFS) {
			kprintf("nvme: controller fatal during disable\n");
			return -1;
		}
	}

	/* Allocate Admin SQ + CQ (one 4 KB page each, plenty for 32
	 * entries: 32*64 = 2 KB SQ, 32*16 = 512 B CQ). */
	d->asq = pmm_alloc();
	d->acq = pmm_alloc();
	if (!d->asq || !d->acq) {
		kprintf("nvme: PMM exhausted during AQ alloc\n");
		return -1;
	}
	kmemset(d->asq, 0, 4096);
	kmemset(d->acq, 0, 4096);
	d->asq_tail = 0;
	d->acq_head = 0;
	d->acq_phase = 1;
	d->next_cid  = 1;

	/* AQA: ASQS + ACQS are "queue size - 1" in entries. */
	uint32_t aqa = (uint32_t)((NVME_AQ_DEPTH - 1) << 16) |
	               (uint32_t) (NVME_AQ_DEPTH - 1);
	mmio32_w(d->regs, NVME_REG_AQA, aqa);
	mmio64_w(d->regs, NVME_REG_ASQ, (uint64_t)(uintptr_t)d->asq);
	mmio64_w(d->regs, NVME_REG_ACQ, (uint64_t)(uintptr_t)d->acq);

	/* CC: IOSQES=6 (1<<6 = 64 B SQE), IOCQES=4 (1<<4 = 16 B CQE),
	 * AMS=000 (round-robin), MPS=0 (4 KB host page), CSS=000
	 * (NVM command set), EN=1. */
	cc = (6u << 16) | (4u << 20) | NVME_CC_EN;
	mmio32_w(d->regs, NVME_REG_CC, cc);
	for (;;) {
		uint32_t csts = mmio32(d->regs, NVME_REG_CSTS);
		if (csts & NVME_CSTS_RDY) break;
		if (csts & NVME_CSTS_CFS) {
			kprintf("nvme: controller fatal during enable "
				"(CSTS=0x%x)\n", csts);
			return -1;
		}
	}

	return 0;
}

/* ---- Identify Controller + Namespace ---- */

static int nvme_identify(struct nvme_dev *d)
{
	void *idbuf = pmm_alloc();
	if (!idbuf) return -1;
	kmemset(idbuf, 0, 4096);

	/* Identify Controller (CNS=1). */
	struct nvme_sqe cmd = { 0 };
	cmd.opc   = 0x06;
	cmd.prp1  = (uint64_t)(uintptr_t)idbuf;
	cmd.cdw10 = 1;
	if (nvme_admin_submit_and_wait(d, &cmd) < 0) {
		kprintf("nvme: Identify Controller failed\n");
		pmm_free(idbuf);
		return -1;
	}
	const struct nvme_id_ctrl *ctrl = idbuf;
	nvme_singleton_info.vid = ctrl->vid;
	for (int i = 0; i < 40; i++)
		nvme_singleton_info.model[i] = ctrl->mn[i] ? ctrl->mn[i] : ' ';
	nvme_singleton_info.model[40] = 0;
	for (int i = 0; i < 20; i++)
		nvme_singleton_info.serial[i] = ctrl->sn[i] ? ctrl->sn[i] : ' ';
	nvme_singleton_info.serial[20] = 0;
	for (int i = 0; i <  8; i++)
		nvme_singleton_info.firmware[i] = ctrl->fr[i] ? ctrl->fr[i] : ' ';
	nvme_singleton_info.firmware[8] = 0;
	kprintf("nvme: ctrl vid=0x%04x model=%s sn=%s fw=%s\n",
		ctrl->vid,
		nvme_singleton_info.model,
		nvme_singleton_info.serial,
		nvme_singleton_info.firmware);

	/* Identify Namespace (CNS=0, NSID=1). */
	kmemset(idbuf, 0, 4096);
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.opc   = 0x06;
	cmd.nsid  = 1;
	cmd.prp1  = (uint64_t)(uintptr_t)idbuf;
	cmd.cdw10 = 0;
	if (nvme_admin_submit_and_wait(d, &cmd) < 0) {
		kprintf("nvme: Identify Namespace failed\n");
		pmm_free(idbuf);
		return -1;
	}
	const struct nvme_id_ns *ns = idbuf;
	uint8_t lbaf_idx = ns->flbas & 0xf;
	uint32_t lbads   = ns->lbaf[lbaf_idx].lbads;
	d->nsze      = ns->nsze;
	d->lba_bytes = 1u << lbads;
	d->nsid      = 1;
	nvme_singleton_info.ns1_blocks    = d->nsze;
	nvme_singleton_info.ns1_lba_bytes = d->lba_bytes;

	kprintf("nvme: ns1 %lu LBAs * %u bytes (%lu MB)\n",
		d->nsze, d->lba_bytes,
		(d->nsze * d->lba_bytes) >> 20);

	pmm_free(idbuf);
	return 0;
}

/* ---- Create I/O queue pair ---- */

static int nvme_create_io_queues(struct nvme_dev *d)
{
	d->iosq = pmm_alloc();
	d->iocq = pmm_alloc();
	if (!d->iosq || !d->iocq) return -1;
	kmemset(d->iosq, 0, 4096);
	kmemset(d->iocq, 0, 4096);
	d->iosq_tail = 0;
	d->iocq_head = 0;
	d->iocq_phase = 1;

	/* Create I/O Completion Queue (admin opc 0x05). */
	struct nvme_sqe cmd = { 0 };
	cmd.opc   = 0x05;
	cmd.prp1  = (uint64_t)(uintptr_t)d->iocq;
	cmd.cdw10 = ((uint32_t)(NVME_IOQ_DEPTH - 1) << 16) | 1;	/* QSIZE-1, QID=1 */
	cmd.cdw11 = 1;	/* PC=1 (physically contiguous), IEN=0 (no IRQs) */
	if (nvme_admin_submit_and_wait(d, &cmd) < 0) {
		kprintf("nvme: Create I/O CQ failed\n");
		return -1;
	}

	/* Create I/O Submission Queue (admin opc 0x01). */
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.opc   = 0x01;
	cmd.prp1  = (uint64_t)(uintptr_t)d->iosq;
	cmd.cdw10 = ((uint32_t)(NVME_IOQ_DEPTH - 1) << 16) | 1;	/* QSIZE-1, QID=1 */
	cmd.cdw11 = (1u << 16) | 1;	/* CQID=1, PC=1, prio=0 */
	if (nvme_admin_submit_and_wait(d, &cmd) < 0) {
		kprintf("nvme: Create I/O SQ failed\n");
		return -1;
	}
	return 0;
}

/* ---- Public entry ---- */

void nvme_init(void)
{
	/* Scan pci_devs[] for class 0x0108 (Mass Storage / NVM
	 * controller).  Prog IF 0x02 in the byte below would narrow
	 * to NVMe specifically, but everything we'd see at that
	 * subclass IS NVMe in practice. */
	struct pci_device *found = NULL;
	for (uint32_t i = 0; i < pci_nr_devs; i++) {
		if (pci_devs[i].class_subclass == 0x0108) {
			found = &pci_devs[i];
			break;
		}
	}
	if (!found) return;

	struct nvme_dev *d = &nvme_singleton;
	kmemset(d, 0, sizeof(*d));

	if (nvme_setup(d, found)         < 0) return;
	if (nvme_identify(d)             < 0) return;
	if (nvme_create_io_queues(d)     < 0) return;

	if (d->lba_bytes != BLK_SIZE) {
		kprintf("nvme: namespace LBA size %u != %u, "
			"skipping block_device wire-up\n",
			d->lba_bytes, (unsigned)BLK_SIZE);
		return;
	}

	d->bd.bd_name    = "nvme0n1";
	d->bd.bd_nblocks = (uint32_t)d->nsze;	/* truncated for now */
	d->bd.bd_read    = nvme_bd_read;
	d->bd.bd_write   = nvme_bd_write;

	nvme_singleton_init = 1;
	nvme_singleton_info.present = 1;
	kprintf("nvme: ready -- nvme0n1 = %u blocks of %u bytes\n",
		d->bd.bd_nblocks, (unsigned)BLK_SIZE);

	/* Self-test: round-trip a known pattern through the LAST LBA.
	 * Picks up controller-bring-up / queue-creation / PRP regressions
	 * the moment they happen, with no userland test rig needed.
	 *
	 * Pre-read the LBA before overwriting and restore the original
	 * contents on the way out: stage F.1 wires /home on top of this
	 * namespace and the kfs superblock cannot tolerate the selftest
	 * pattern landing on LBA 0.  We use the highest LBA partly so
	 * kfs's growing bitmap won't reach it in normal use, but also
	 * write-then-restore so even a tightly-packed namespace stays
	 * byte-identical across boots. */
	void *buf  = pmm_alloc();
	void *save = pmm_alloc();
	uint32_t test_lba = d->bd.bd_nblocks - 1;
	if (buf && save) {
		int ok = 0;
		uint8_t *p = buf;
		if (nvme_bd_read(&d->bd, test_lba, save) < 0) {
			kprintf("nvme: selftest pre-read FAIL\n");
		} else {
			for (int i = 0; i < BLK_SIZE; i++)
				p[i] = (uint8_t)(0xA5 ^ i);
			if (nvme_bd_write(&d->bd, test_lba, buf) < 0) {
				kprintf("nvme: selftest write FAIL\n");
			} else {
				kmemset(buf, 0, BLK_SIZE);
				if (nvme_bd_read(&d->bd, test_lba, buf) < 0) {
					kprintf("nvme: selftest read FAIL\n");
				} else {
					ok = 1;
					for (int i = 0; i < BLK_SIZE; i++) {
						if (p[i] != (uint8_t)(0xA5 ^ i)) {
							ok = 0; break;
						}
					}
				}
				/* Always try to put the original bytes back,
				 * even on a data mismatch -- otherwise a
				 * single selftest failure permanently corrupts
				 * the disk. */
				(void)nvme_bd_write(&d->bd, test_lba, save);
			}
		}
		kprintf("nvme: selftest %s\n",
			ok ? "PASS" : "FAIL (data mismatch)");
	}
	if (buf)  pmm_free(buf);
	if (save) pmm_free(save);
}

struct block_device *nvme_get(void)
{
	if (!nvme_singleton_init) return NULL;
	return &nvme_singleton.bd;
}

void nvme_get_info(struct nvme_info *out)
{
	*out = nvme_singleton_info;
}

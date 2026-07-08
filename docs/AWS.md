# Porting kappara to AWS Graviton

This is the roadmap for getting kappara to boot on an EC2
`a1.medium` / `t4g.nano` style instance.  We're not trying to be
Linux; we just need to satisfy the same UEFI + ACPI + PCIe
contract real Linux satisfies, because that's what the platform
exposes.

Boot chain we're targeting:

    EC2 instance launch
        ↓
    AWS Nitro UEFI firmware
        ↓
    GRUB (or kappara as a direct EFI app)
        ↓
    kappara kernel  (this part is new)
        ↓
    ACPI table parse  →  GIC v3 + GT + memory map
        ↓
    PCIe ECAM enumeration
        ├── ENA  (network)
        └── NVMe (root disk)
        ↓
    user-space init

Status convention is the same as `docs/FTPD.md` /
`docs/DYNAMIC.md`: each stage flips `[ ]` to `[x]` when its work
lands.

## Architecture choices (locked)

- **One arch dir: `arch/aws64/`** (eventually).  Reuses every line
  of `uts/os/` that's not platform-specific (sched, vfs, streams,
  the whole TCP stack, every cmd binary).  Same shape as
  `arch/virt/` vs `arch/aarch64/` today.
- **No DTB.**  AWS doesn't publish one; firmware passes ACPI.
- **Boot as a Linux ARM64 Image** (the PE32+ stub flavour) so
  GRUB picks us up unmodified.  Our `kernel.img` grows a 64-byte
  Image header at offset 0.
- **MMU on at handoff.**  EFI hands us control with the MMU
  already enabled in identity-mapped mode; we have to migrate
  the page tables to our own layout after ExitBootServices.
- **MSI-X for PCIe IRQs.**  The legacy INTx route doesn't work in
  Nitro; everything is message-signalled.

## Stages

### Stage A -- Linux ARM64 Image header

Status: `[x]`

Boot stub written in asm: 64 bytes prefixed to our raw binary
with the right magic at offset 56 (`ARM\x64`), text_offset,
image_size, flags (4 KB pages, anywhere placement), and a `b _start`
at offset 4.

What landed:

- `uts/virt/boot.S` already carried a placeholder Image header
  (QEMU's `-kernel` checks for the "ARM\x64" magic at offset 0x38
  to identify the image).  Stage A fills in the rest of the
  fields: `text_offset = 0x80000` (Linux convention),
  `image_size = __kernel_end - __kernel_start`, `flags = 0x0A`
  (bit 1 = 4K pages, bit 3 = "anywhere" placement).
- `uts/virt/linker.ld` exports `_kernel_image_size_lo32` and
  `_kernel_image_size_hi32` via `ABSOLUTE(...)`.  The assembler
  can't compute the linker-time difference, so we emit two
  `.long` references the linker resolves into the right
  little-endian 64-bit field.  Same trick Linux head.S uses.
- code0 is still `add x13, x18, #0x16` which encodes as bytes
  `4d 5a 00 91` -- "MZ\x00\x91".  The first two bytes are the
  MS-DOS PE magic so EFI accepts us; the AArch64 instruction
  itself is a harmless no-op (writes to scratch x13 we never
  read).  PE COFF offset at byte 60 stays 0 until stage B
  points it at the EFI PE header.

Header bytes (`od -An -tx1 -N64 build/kernel.img`):

    4d 5a 00 91 0f 00 00 14 00 00 08 00 00 00 00 00
    00 40 78 00 00 00 00 00 0a 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 41 52 4d 64 00 00 00 00
                                        ^^ "ARM\x64"

This stage does NOT yet add EFI parsing.  After this stage we
still don't boot on AWS -- but we produce a binary that GRUB
and other Linux-aware loaders are willing to drop into RAM at
the right address, the prerequisite for stage B.

Verified: `make test` ALL TESTS PASS.  The same kernel image
QEMU has been booting via `-kernel` is unchanged in behaviour;
the header metadata fields now carry real values.

### Stage B -- EFI app + ExitBootServices

Status: `[x]`

Make `kernel.img` actually be a valid PE32+ executable.  The
Linux trick is to use the same 64-byte header as both the ARM64
Image header AND the start of an MZ-flavoured PE32+ header (the
magic overlap is real; PE's first two bytes "MZ" coincide with
the AArch64 `add x13, x18, #0x16` instruction, encoded as
`4d 5a 00 91`).

What landed:

- `uts/virt/boot.S` grew a PE32+ header block immediately after
  the 64-byte Linux Image header: PE signature, COFF header
  (Machine=0xAA64, Characteristics=0x0207 = RELOCS_STRIPPED |
  EXECUTABLE_IMAGE | LINE_NUMS_STRIPPED | DEBUG_STRIPPED), PE32+
  optional header (Magic=0x020B, ImageBase=0x40080000,
  SectionAlignment=0x1000, FileAlignment=0x200, Subsystem=0x0A
  EFI_APPLICATION, NumberOfRvaAndSizes=16), and a single `.text`
  section descriptor.  `.balign 0x1000` after the section table
  pads the header region to SizeOfHeaders so the .text section
  starts at file offset 0x1000.
- `uts/virt/linker.ld` exposes `_text_rva`, `_pe_size_of_headers`,
  and `_text_section_size_lo32` for those header fields.  EDK II
  requires the full 16-entry DataDirectory even though we leave
  all entries zero.
- `uts/virt/efi.h` + `uts/virt/efi_main.c` implement
  `efi_main(handle, system_table)`:
  1. Walks `system_table->tables` (the EFI Configuration Table)
     for ACPI 2.0 GUID `8868e871-e4f1-11d3-bc22-0080c73c8881`,
     stashing the RSDP in `efi_acpi_rsdp` (BSS, read by kmain).
  2. Calls `BootServices->GetMemoryMap` into a 4 KB BSS buffer
     (`efi_memmap_storage`); records size, descriptor_size,
     and descriptor_version for stage C.
  3. Calls `BootServices->ExitBootServices` with the returned
     map key.  After this we own the machine.
- `boot.S` adds an `efi_pe_entry` stub at the start of the .text
  section (so its RVA is inside a section per the PE spec).
  It saves x0/x1 (image handle, system table) to x19/x20, sets
  sp to `stack_top`, calls `efi_main`, then on success branches
  to the regular Linux Image entry path (`.Lreal_start`); on
  failure spins in WFI.
- `tools/pad_pe.py` post-processes `kernel.img` after `objcopy
  -O binary` to zero-pad the file out to SizeOfImage.  Without
  this EDK II's PE loader sees SizeOfRawData reaching past
  end-of-file and rejects the image with
  "Script Error Status: Unsupported".

What this does NOT do: anything device-specific.  Stage C will
walk the ACPI pointer + EFI memory map we stashed; for now
under UEFI we hand off to `.Lreal_start` and rely on the
pre-existing identity-mapped early-MMU state (cleanup is on
the stage C TODO).

Verified: under AAVMF + QEMU virt the kernel loads and the
firmware jumps to it (synchronous exception fires in our RX
region at the expected handover point, as expected without
stage C's ACPI/MMU rework).  `make test` still reports ALL
TESTS PASS via the `-kernel` path -- the regular dev workflow
is unchanged.

### Stage C -- ACPI parser

Status: `[x]`

Minimal ACPI table walker -- no AML.  Driven by the RSDP pointer
that stage B stashed when efi_main walked the EFI Configuration
Table.  Lives in `uts/virt/acpi.[ch]`; entry point is
`acpi_init()`, called from kmain right after `mmu_init`.

What landed:

- **RSDP**: validated against "RSD PTR " signature, ACPI 2.0+
  revision, and BOTH the 20-byte (1.0 compat) and full-length
  checksums.  Failures log and bail rather than wire later stages
  off bogus data.
- **XSDT**: signature + checksum validated; entries iterated by
  4-char signature lookup.
- **MADT** ("APIC"): sub-entries walked by type.
    - GICC (0x0B) per CPU -- arm_mpidr + Enabled flag, recorded
      into `acpi_cpu_mpidr[]`/`acpi_nr_cpus` (cap ACPI_MAX_CPUS=32).
    - GICD (0x0C) -- `acpi_gicd_base`, `acpi_gic_version`.
    - GICR (0x0E) -- `acpi_gicr_base`/`acpi_gicr_length`.
    - MSI/ITS skipped for now; stage E reads them.
- **MCFG**: first allocation's ECAM base + bus range recorded
  (`acpi_pcie_ecam_base`, `acpi_pcie_bus_start/end`).
- **GTDT**: non-secure EL1 + virtual EL1 timer GSIVs recorded
  for stage D's dynamic timer wire-up.
- **FADT** ("FACP"): flags word stashed; rest deferred.

Stage C only reads + prints (one `acpi:` line per table).
Hardcoded `PLAT_GIC_*` constants in `uts/aarch64/platform/virt.h`
still drive the running kernel.  Stage D will swap those for the
discovered values + walk MCFG to enumerate PCIe.

Boot path coverage:

- UEFI boot: full parse; logs RSDP/XSDT addresses, GIC version,
  CPU MPIDRs, ECAM base + bus range, timer GSIVs.
- `-kernel`: `efi_acpi_rsdp` is NULL; `acpi_init` logs one
  "no RSDP" line and returns.  Verified -- `make test` still
  reports ALL TESTS PASS.

### Stage D -- PCIe ECAM bus enumeration

Status: `[x]`

Walk PCIe bus 0..255 / dev 0..31 / fn 0..7 using the ECAM base
from MCFG.  Lives in `uts/virt/pcie.[ch]`; entry point
`pcie_init()` called from kmain right after `acpi_init`.

What landed:

- ECAM math: `ecam_base + (bus<<20) + (dev<<15) + (fn<<12)` for
  each function's 4 KB config window.  Endpoint records go into
  `pci_devs[]` (cap `PCI_MAX_DEVS=32`): vendor/device IDs,
  class+subclass, prog-if, revision, header type, all 6 BAR
  values (raw, not sized), and the MSI-X capability offset (0
  if absent).
- Capability list walked from offset 0x34 looking for cap id
  0x11 (MSI-X); cap chain bounded to 48 hops to defend against
  malformed devices.
- AWS hardware IDs called out by name in the per-device log line
  (Amazon vendor `0x1d0f`: ENA `0xec20`, NVMe `0x8061`).
- Multi-function devices probed by checking header-type bit 7
  on fn 0.

Two non-obvious things this stage needed before ECAM reads
could even land:

- `uts/aarch64/mmu.c` TCR_EL1.IPS bumped from 36-bit (64 GB) to
  44-bit (16 TB) PA.  QEMU virt's highmem PCIe ECAM sits at
  `0x4010000000` -- well above 36 bits -- and tried to MMIO-read
  it caused an Address Size Fault at L1 (ESR DFSC = 0x01).
- New `mmu_map_device_1gb(va)` helper (mmu.h + mmu.c) writes a
  1 GB Device-nGnRE L1 block for the ECAM base; `pcie_init`
  calls it before any config-space load so the 1 GB window is
  reachable.  Boot identity map alone only covers 0..2 GB.

Boot-path coverage:

- UEFI (AAVMF): real ACPI MCFG present, full enumeration runs.
  Verified with `-device virtio-blk-pci`: finds host bridge
  (`1b36:0008` class 0x0600) and the virtio-blk endpoint
  (`1af4:1001` class 0x0100).
- `-kernel`: no MCFG, one-line skip and return.  We do NOT
  fall back to QEMU virt's static ECAM base (0x3F000000) -- the
  region is reserved in the memory map but gpex returns
  synchronous external aborts for unmapped buses, breaking the
  walk.  Trade-off accepted: dev exercise runs through ACPI;
  raw `-kernel` developers can still iterate on the rest of
  the kernel without PCI.

Two parallel boot.S corrections lived alongside Stage D because
they had to ship together to actually exercise the UEFI path:

- PE section characteristics changed from RX (`0x60000020`) to
  RWX (`0xE0000020`).  BSS lives in this single section and
  efi_main's first stack push faulted inside EFI's mapping.
- Boot path after a successful `efi_main` now jumps to a new
  `.Lpost_bss` label, skipping the BSS-clear loop that was
  wiping `efi_acpi_rsdp` / `efi_memmap_*` between efi_main
  finishing and kmain starting.  BSS is already zero in the
  loaded image because `objcopy -O binary` + `tools/pad_pe.py`
  zero-fill the on-disk file out to SizeOfImage.

### Stage E -- ENA network driver

Status: `[~]` (register offsets and admin protocol verified against
Linux `ena_regs_defs.h` / `ena_admin_defs.h`; reset + admin queue +
GET_FEATURE + I/O queue creation should be correct; TX/RX packet
paths are skeletons -- see "What remains" below).

ENA = AWS Elastic Network Adapter; PCI vendor 0x1d0f, device
0xec20 (or 0xec21 on some VFs), class 0x0200.  Lives in
`uts/virt/ena.c`; entry point `ena_init()` runs from kmain right
after `nvme_init()`.

Structural shape (matches the upstream Linux driver in
`drivers/net/ethernet/amazon/ena/`):

1. PCI probe for the Amazon vendor + class-network pair.
2. PCI Command register: enable Memory Space + Bus Master
   (same dance nvme.c does -- UEFI leaves those clear).
3. Map BAR0 (registers) via `mmu_map_device_1gb`.
4. Reset: check DEV_STS.READY, write DEV_CTL.RESET twice
   (spec requirement for RESET_REASON field), poll
   DEV_STS.RESET_IN_PROGRESS then RESET_FINISHED, clear
   DEV_CTL, wait DEV_STS.READY.
5. Allocate Admin SQ + Admin CQ + AENQ pages from the PMM;
   program AQ_BASE / CAPS, ACQ_BASE / CAPS, AENQ_BASE / CAPS.
6. Mask all interrupts (polled driver).
7. `GET_FEATURE(DEVICE_ATTRIBUTES)` admin command -> MAC, MTU,
   max queue counts.
8. `CREATE_CQ` + `CREATE_SQ` for one TX pair and one RX pair.
9. Pre-fill the RX SQ with PMM-backed buffers; ring the RX
   doorbell.
10. Register a `struct netif` named `eth0` so the IP layer sees us.

What WORKS (mechanically -- not tested on hardware yet):
- Probe + reset + admin queue + GET_FEATURE + queue creation
  paths all compile + run on the non-ENA fallback path (init
  is a no-op when the device is absent).
- `-kernel` boot under QEMU virt is unchanged; ENA never
  matches because QEMU has no ENA emulation.

#### Verified register constants

All register offsets and bit definitions were cross-checked
against `torvalds/linux drivers/net/ethernet/amazon/ena/`:

- BAR0 register map: `ENA_REGS_*_OFF` -- all offsets correct,
  including the `INTR_MASK` at 0x4C and `DEV_CTL`/`DEV_STS` at
  0x54/0x58 (the original skeleton had these 12 bytes too low).
- DEV_STS bits: READY=0x01, RESET_IN_PROGRESS=0x08,
  RESET_FINISHED=0x10, FATAL_ERROR=0x20.
- Admin queue entry sizes: SQ=64B (4B header), CQ=64B (8B header),
  AENQ=64B -- all verified.
- ACQ phase bit: `flags` bit 0 (not `command` bit 12 as the
  original skeleton assumed).
- CQ entry size differs per direction: TX completion is 2 words
  (8 B), RX completion is 4 words (16 B).  `CREATE_CQ` must say
  which one; the firmware rejects a TX SQ pointed at a 4-word CQ
  with `RESOURCE_BUSY` (status 6).
- `CREATE_SQ` response: `sq_doorbell_offset` is the u32 at
  payload +4 (byte offset from BAR0).  Payload +8 is
  `llq_descriptors_offset`, which reads 0 in host-memory mode --
  misreading it sends every doorbell write to BAR0+0
  (`ENA_REG_VERSION`) and the device never sees a single TX
  descriptor (verified on Nitro: `tx sq_db=0x0`).
- `CREATE_CQ` response: `cq_head_db_register_offset` is the u32
  at payload +8 and is usually 0 (no CQ doorbell needed for a
  polling consumer); the driver only writes it when nonzero.
- I/O descriptors follow `ena_eth_io_defs.h`, not a naive
  packed-field guess.  TX descriptor is two control words:
  `len_ctrl` ([15:0] length, [21:16] req_id_hi, [24] **phase**,
  [26] first, [27] last, [28] comp_req) + `meta_ctrl`
  ([26:22] req_id_lo; the low bits are offload knobs -- writing
  req_id there sets random csum/TSO flags).  RX descriptor puts
  phase/first/last/comp_req in a ctrl byte at offset 3 and
  req_id at offset 4.  Both SQs carry a per-pass phase bit that
  starts at 1 and flips on each ring wrap -- the device silently
  ignores any descriptor whose phase doesn't match, which
  presents as "doorbell rings, nothing happens".
- RX completion (`ena_eth_io_rx_cdesc_base`): phase is
  status[24]; `length` at +4, `req_id` at +6.
- DHCP reply sizes: QEMU slirp pads BOOTP replies to the full
  312-byte options field; the EC2 VPC responder sends compact
  frames (~300 bytes total).  The RX filter must only require the
  fixed BOOTP header through the magic cookie (240 bytes past
  UDP) and bound the options walk by the received length --
  requiring `sizeof(struct dhcp_pkt)` silently rejects every real
  OFFER while passing all slirp-based tests.  On DHCP failure the
  driver prints tx sent/done + rx got and hexdumps the first
  unmatched frame.

Once the offsets are pinned, what remains for "real packet
I/O" (not just queue creation):
- RX kthread that drains the CQ, hands each completion's
  buffer up to the IP stack as an mblk, refills the SQ.
- TX path: build TX descriptor from an mblk's b_rptr/b_wptr,
  ring the doorbell, free the mblk on completion.
- DHCP wire-up (currently `ip = 0` -- DHCP discovery runs in
  the virtio-net path; needs lifting into a shared helper).

### Stage F -- NVMe block driver

Status: `[x]` (driver live; kfs-on-NVMe mount comes later)

Polled NVMe 1.4 driver.  Lives in `uts/virt/nvme.[ch]`; entry
point `nvme_init()` called from kmain after `ramdisk_*_init` and
before `exec_space_init`.  Detects the controller by PCI class
0x0108 (Mass Storage / NVM controller), not by vendor:device --
QEMU's NVMe is `1b36:0010`, Amazon EBS is `1d0f:8061`, both match.

What landed:

- Controller bring-up follows NVMe 1.4 §3.5.1: disable, wait
  CSTS.RDY=0, allocate Admin SQ/CQ from PMM, program AQA/ASQ/ACQ,
  set CC (IOSQES=6, IOCQES=4, CSS=NVM, MPS=4KB, EN=1), wait
  CSTS.RDY=1.
- PCI Command register Memory-Space + Bus-Master bits set
  explicitly before any MMIO -- UEFI's PCI bus driver leaves
  those clear on devices it didn't drive itself, and without
  them the BAR window reads back all-ones.
- Identify Controller + Identify Namespace (NSID=1); model,
  serial, firmware, namespace LBA count + size recorded into
  `nvme_singleton_info` for `/proc/nvme`.
- One I/O queue pair created (admin opc 0x05 + 0x01), depth 32.
- `struct block_device` adapter named `nvme0n1` wired with
  bd_read / bd_write that submit a single NVM Read (0x02) or
  Write (0x01) per call (NLB=0), polled.  Only 512-byte LBAs
  are accepted -- 4 KB namespaces would need translation.
- Built-in self-test runs at the end of nvme_init: writes a
  known pattern to LBA 0, reads it back, byte-compares.  Logs
  `nvme: selftest PASS` -- catches regressions in the entire
  bring-up + queue path the moment they happen.

Two infrastructure pieces landed alongside:

- `mmu_map_device_1gb()` now walks L0 too and lazily allocates
  a fresh L1 page from the PMM for L0[1+] entries.  Required:
  QEMU virt's highmem PCIe MMIO sits at 0x8000000000 (= 512 GB,
  the very start of L0[1]).  Previous version masked the L1
  index to 9 bits and would have stomped L1[0] (the low 1 GB
  identity map).
- `pcie_bar_addr()` + `pcie_bar_is_64()` helpers in
  `kappara/arch/pcie.h` -- decode 32 vs 64-bit memory BARs
  (low/high split) so drivers don't repeat the bit-twiddling.

Verified end-to-end under AAVMF + QEMU virt with
`-device nvme,drive=nvm,serial=...`:
  - probe + Identify works
  - 512 B selftest passes
  - host-side `nvme.img` file contains the kernel-written pattern
    (byte[i] = 0xA5 ^ i) -- persistence proven across qemu
    process boundary
  - `/proc/nvme` returns the controller summary (version, vid,
    model, serial, firmware, namespace size)

`make test` still ALL TESTS PASS (14/14) on the `-kernel` path
where there's no NVMe device (driver is a no-op).

What's still missing (next iteration, not blocking stage G):

- kfs-on-nvme mount (boot `/usr/bin` + `/home` off EBS instead
  of in-RAM ramdisks).  The block_device is registered; it
  just isn't wired into `kfs_mount` yet.
- MSI-X interrupts.  We poll today; on a real Graviton workload
  we'd want to sleep on completion via a per-queue wait queue.
- Multi-block transfers + PRP2 / SGL.  Today's bd_read/write
  is one LBA per call.

### Stage F.1 -- kfs `/home` on NVMe / EBS

Status: `[x]` (mount + first-boot mkimage; persistent across reboots)

Mount kfs on top of `nvme0n1` for `/home`.  Stays on the in-RAM
`ramdisk0` for `/usr/bin` (those are immutable kernel-embedded
ELFs; persistence there is meaningless until we move the build
to "ship binaries via AMI" in stage G).

What landed in `uts/os/user/user.c::exec_space_init`:

- If `nvme_get()` returns a block_device, use it for `/home`;
  otherwise fall back to `ramdisk_home_get()`.
- First-boot logic: try `kfs_mount` straight away.  If it
  reports bad magic (= raw / freshly created namespace),
  `kfs_mkimage` it as an empty kfs, then mount.  On a
  reboot with a populated disk the first mount succeeds
  and no mkimage runs -- preserving everything.

Two bugs landed alongside, both regressed by stage F:

- `mmu_vmap_create` now copies every populated `l0_table[1+]`
  entry into per-process L0s, not just the kernel L1[1].  Stage
  F's `mmu_map_device_1gb` for the NVMe BAR at 0x8000000000
  populates `l0_table[1]`; without inheriting that, the first
  exec'd user thread doing kfs I/O page-faults inside
  `nvme_io_submit_and_wait`'s doorbell write.

- `nvme_init`'s selftest no longer clobbers LBA 0.  It now
  pre-reads the LAST LBA, writes the pattern there, reads
  back, then restores the original bytes -- so the round-trip
  is byte-neutral on the disk and the kfs superblock (block 0)
  survives.

End-to-end persistence verified under AAVMF + virt + a real
`nvme.img`:

```
phase 1 (fresh disk): touch /home/hello; echo /home/hello text; halt
phase 2 (same img):   cat /home/hello   -> "text"
```

The phase-2 log shows `exec: /home mounted from nvme0n1
(persistent)` rather than `formatted + mounted` -- proving the
on-disk superblock is recognised and we skipped the mkimage
fallback.

### Stage G -- Sample AMI build

Status: `[x]`

`make ami` (after the single-arch collapse, no more `ARCH=`)
produces `build/kappara-ami.img`: a 130 MiB raw disk with a GPT
partition table and one EFI System Partition (FAT32, ~128 MiB)
containing `\EFI\BOOT\BOOTAA64.EFI` = our kernel.img.

Layout:

```
GPT header  +  ESP partition (FAT32, labelled "KAPPARA")
1 MiB          ^ \EFI\BOOT\BOOTAA64.EFI = kernel.img
               | (PE32+ EFI app w/ embedded ARM64 Image header)
```

The kernel is the same `build/kernel.img` that QEMU `-kernel`
boots; UEFI uses its PE32+ header (stage B), QEMU uses its
Linux ARM64 Image header (stage A).  No code duplication.

`/home` (stage F.1) is NOT carved into this image -- on AWS,
mutable data goes on a separate EBS volume so the root volume
stays immutable.  At instance launch:

1. Boot volume: this AMI (read-only most of the time).
2. Data volume: a second EBS volume, attached as NVMe.  Empty
   on first boot; stage F.1's "kfs_mount or kfs_mkimage"
   handler formats it.  Subsequent reboots preserve files.

Local smoke-boot under QEMU + AAVMF (drop-in equivalent of how
EC2 fires the AMI): `make ami-run`.  Brings up the AMI image as
the boot disk + a small blank file as the second NVMe volume;
verified to reach the `kappara:/#` prompt and persist a write
to `/home`.

The tool that builds the image is `tools/make-ami.sh`; it uses
`parted` + `dosfstools` + `mtools`.  None of these run during a
normal `make`; only when you explicitly ask for an AMI.

#### Pushing to AWS

The repo doesn't automate this end-to-end yet -- the assumption
is you'd run these by hand once per release.  Pseudo-recipe:

```
# 1. Build the disk image
make ami

# 2. Upload the raw image to an S3 bucket
aws s3 cp build/kappara-ami.img s3://YOUR_BUCKET/kappara-ami.img

# 3. Tell EC2 to import it as a snapshot (creates an EBS-backed
#    snapshot from the raw blob).  Needs an IAM role granting
#    VMIE access; see AWS's "vmimport" service-role docs.
TASK=$(aws ec2 import-snapshot --description "kappara root" \
       --disk-container file://<(printf '%s' \
         '{"Format":"raw","UserBucket":{"S3Bucket":"YOUR_BUCKET",'\
         '"S3Key":"kappara-ami.img"}}') \
       --query ImportTaskId --output text)

# 4. Poll until completed; grab the resulting SnapshotId
aws ec2 describe-import-snapshot-tasks --import-task-ids $TASK

# 5. Register the snapshot as an AMI (aarch64, UEFI boot)
aws ec2 register-image --name kappara-r0 --architecture arm64 \
    --boot-mode uefi --root-device-name /dev/sda1 \
    --block-device-mappings 'DeviceName=/dev/sda1,Ebs={SnapshotId=snap-XXX}'

# 6. Launch on a c7g / t4g (Graviton) instance.  Attach an extra
#    EBS volume for /home; kappara will format + mount it.
```

What's intentionally left for later:

- A real `boot-mode=uefi-preferred` story so the AMI also works
  on instance types that allow legacy fallback (unlikely to
  matter -- Graviton is UEFI-native).
- ENA driver (stage E) -- without it the AMI runs but has no
  network on the instance.  QEMU virt papers over this with
  virtio-net; EC2 doesn't.

## Dependency chain

```
DYNAMIC.md stages              AWS.md stages
    1 libc fill-out
    2 libc PIC
    3 cmd PIE
    4 loader relocations
    5 ld-kappara.so       ← independent of AWS
    6 libc.so

                              A Image header
                              B EFI + ExitBootServices
                              C ACPI parser
                              D PCIe enumeration
                              E ENA driver
                              F NVMe driver + EBS root
                              G sample AMI
```

The two roadmaps are independent.  Dynamic linking gives us a
better userland; AWS gives us a real deployment target.  Doing
both makes kappara feel like "a real OS" -- but neither blocks
the other, so we can interleave by appetite.

## What this is NOT

- A Linux compatibility layer.  Nothing built for Linux runs
  here.  No `binfmt_misc`, no `libc.so` ABI compatibility.
- A POSIX-conformant OS.  We have a kappara-specific syscall
  ABI; running OpenSSH or PostgreSQL is way beyond scope.
- Multi-architecture.  AWS x86 isn't a goal; only Graviton (ARM).

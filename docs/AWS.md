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

Status: `[ ]`

ENA is documented in the Amazon Linux source tree (Apache 2.0).
Protocol shape:

- Admin queue for management commands.
- Per-direction (Tx / Rx) queue pairs, each with a submission
  ring + completion ring.
- MSI-X vector per queue.

We wire ENA up under the existing `struct netif` shape -- it
takes the same `tx` / `rx` slot virtio_net.c provides; everything
above (`ip_attach_stream` etc) stays unchanged.

After this stage `make ARCH=aws64 run-telnet` works for a real
EC2 instance with a public IP.

### Stage F -- NVMe block driver + EBS root

Status: `[ ]`

NVMe is industry standard.  Single namespace (NSID=1), 4 KB
blocks, polled completion to start (MSI-X later if needed).

We wire NVMe under the existing `struct block_device` shape so
kfs mounts it the same way it mounts our ramdisks today.  Big
deal: `/usr/bin` and `/home` come off EBS instead of being baked
into the kernel image.  Survives reboots.

### Stage G -- Polish + sample AMI build

Status: `[ ]` (optional)

`make ARCH=aws64 ami` produces a raw disk image with our kernel
+ a FAT EFI System Partition + a kfs root.  Upload to S3, register
as an AMI, launch.

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

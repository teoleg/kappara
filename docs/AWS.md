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

Status: `[ ]`

Boot stub written in asm: 64 bytes prefixed to our raw binary
with the right magic at offset 56 (`ARM\x64`), the text offset,
the image size, flags (4 KB pages, little-endian, no swap), and
a `b _start` at offset 0.  No real code.  GRUB only needs the
header to load us.

This stage does NOT yet add EFI parsing.  After this stage we
still don't boot on AWS -- but we can produce a binary that
GRUB-style loaders are willing to drop into RAM at the right
address, which is the prerequisite for everything below.

Build target: `build/aws64/kernel.img` (raw bytes) gets a header
prefix; downstream targets get a `.efi` variant separately.

### Stage B -- EFI app + ExitBootServices

Status: `[ ]`

Make `kernel.img` actually be a valid PE32+ executable.  The
Linux trick is to use the same 64-byte header as both the ARM64
Image header AND the start of an MZ-flavoured PE32+ header (the
magic overlap is real; PE's first two bytes "MZ" coincide with
the start of the `b _start` AArch64 jump instruction at the
right opcode pattern).

What this entails:

- Linker script grows a `.text` section for the PE header
  literals (just bytes).
- A small `efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE *)` C function
  that:
  1. Walks the EFI Configuration Table looking for an ACPI 2.0
     entry (UUID `8868e871-e4f1-11d3-bc22-0080c73c8881`); stashes
     the RSDP.
  2. Calls `BootServices->GetMemoryMap` to learn the physical
     ranges.
  3. Calls `BootServices->ExitBootServices`.
  4. Disables the EFI MMU mapping (or trusts it) and hands
     control to the kappara `kmain` -- but with the ACPI pointer
     and the memory map preserved.

What this does NOT do: any device discovery.  We get to kmain,
print "ACPI at 0x..., usable RAM at 0x..-0x..", spin.

### Stage C -- ACPI parser

Status: `[ ]`

Minimal ACPI table walker.  We don't need a full AML interpreter
(that's a different order of magnitude); we only need the static
tables:

- **RSDP** -- root pointer, we already have it from EFI config table.
- **XSDT** -- table of pointers to other tables, walked by name.
- **MADT** -- GIC v3 distributor base, redistributor base,
  per-CPU info.  Today's `virt` uses hardcoded constants; here
  we read them.
- **MCFG** -- PCIe ECAM base address (config space lives at
  ECAM_base + (bus<<20) + (dev<<15) + (fn<<12)).
- **GTDT** -- generic timer info (CNTFRQ, IRQ numbers).
- **FADT** -- random platform flags + IO ports we mostly ignore.

After this stage we can wire up GIC + timer dynamically.  No new
device drivers yet.

### Stage D -- PCIe ECAM bus enumeration

Status: `[ ]`

Walk PCIe bus 0..255 / dev 0..31 / fn 0..7 using the ECAM base
from MCFG.  For each visible device:

- Read Vendor ID / Device ID; recognise:
  - 0x1d0f / 0xec20 -- AWS ENA
  - 0x1d0f / 0x8061 -- AWS NVMe (EBS)
- Read BARs, MSI-X capability, configure interrupt vectors.

After this stage we print "PCIe: found 2 devices" -- nothing's
driving them yet.

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

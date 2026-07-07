# kappara

[![CI](https://github.com/teoleg/kappara/actions/workflows/build.yml/badge.svg)](https://github.com/teoleg/kappara/actions/workflows/build.yml)
[![AMI deploy](https://github.com/teoleg/kappara/actions/workflows/deploy.yml/badge.svg?branch=main)](https://github.com/teoleg/kappara/actions/workflows/deploy.yml)

A small SVR4-flavored Unix-like operating system for AArch64.  Develops on
QEMU `virt` (`qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72`)
and is on the runway to boot on AWS Graviton (EC2 EFI / UEFI) via the
staged transition described in `docs/AWS.md`.

The same `build/kernel.img` boots two ways:

- `qemu-system-aarch64 -kernel build/kernel.img` (dev loop, fast): QEMU
  consumes the Linux ARM64 Image header at offset 0.
- UEFI / AAVMF (Graviton, AAVMF in QEMU): the same 64 bytes double as a
  PE32+ DOS header (`MZ` is an AArch64 `add x13, x18, #0x16`), the rest
  of the PE/COFF + section table is laid out by `uts/virt/boot.S`, and
  EDK II jumps to our `efi_pe_entry`.

The kernel uses SVR4 STREAMS for all character I/O — pipes, ttys, /dev/loop,
/dev/klog, /proc/*, the network stack — with module push/pop, queues, and
message blocks matching the AT&T conventions (mblk_t / qinit / streamtab).
Drivers are discovered via `cdevsw[major]`.  Inode lifecycle follows vnode
`v_count` and `vop_inactive`.  Signals are reliable (DEC/BSD-style:
persistent handlers, sigmask, sigaction).  Numbering is POSIX
(SIGTERM=15, SIGKILL=9, ...).

Networking is a STREAMS multiplexor: `/dev/ip` is the mux, ICMP / UDP /
TCP are pushable modules above it, lo0 / virtio-net / SLIP are stream
drivers underneath.  TCP is the RFC 793 full state graph (LISTEN through
TIME_WAIT) with a multi-accept backlog, RFC 6298 RTT estimation,
exponential-backoff retransmit, and real receive-window advertisement
driven by STREAMS backpressure.  See `docs/ARCHITECTURE.md` for the TCP
phase table.

Userspace boots through a small freestanding libc (`lib/libc/`) and an
in-tree dynamic linker (`lib/ld-kappara/`) — `cmd/*` programs run from
the in-RAM kfs as PIE ELFs, dlopen works against `lib/libdltest`, and
Linux-ABI static-pie binaries built with raw aarch64 syscalls run
unmodified on top of the syscall shim.

No soup for you.

## Quick start

Install cross toolchain + QEMU:

```
sudo apt-get install gcc-aarch64-linux-gnu qemu-system-aarch64
```

Build and run:

```
make            # produces build/kernel.img
make run        # boot under QEMU virt, headless, on stdio
```

You'll get the `kappara:/#` shell.  Type `help` to see commands.  Boot
to prompt is ~3 seconds on QEMU TCG.

To quit:
- **QEMU stdio escape:** `Ctrl-A` then `x` — note that if you're in
  `screen` or `tmux`, those eat `Ctrl-A` by default; use `Ctrl-A a x`
  in screen, or the tmux prefix to escape it.
- **Last resort:** `make stop` from another terminal kills any running QEMU.

To verify the tree is healthy:

```
make test       # smoke-ftp + smoke-sdk + smoke-linux + smoke-linux-mmap
                # + cmd/test all 14/14.  ~1 minute, all from one make.
```

## Targets

| Target            | What it does                                                       |
|-------------------|--------------------------------------------------------------------|
| `make`            | Build `build/kernel.img` (single arch: QEMU virt → AWS Graviton)   |
| `make run`        | Boot under default QEMU args (virt, gic v3, cortex-a72, headless)  |
| `make run-telnet` | Boot with telnetd reachable on `localhost:2323`                    |
| `make test`       | Full regression: 4 smoke binaries + `cmd/test` (14 cases)          |
| `make stop`       | `pkill` any running QEMU process                                   |
| `make clean`      | Remove `build/`                                                    |
| `make TRACE=1 ...`| Build with the ftrace cyg-profile hooks enabled                    |
| `make ami`        | Produce `build/kappara-ami.img` -- a GPT+ESP raw disk suitable for EC2 Graviton AMI import (AWS.md stage G) |
| `make ami-run`    | Smoke-boot the AMI image under QEMU + AAVMF (UEFI), with a blank `/home` NVMe namespace attached |

## What's inside

| File / dir                     | Role                                                              |
|--------------------------------|-------------------------------------------------------------------|
| `uts/virt/boot.S`              | Reset vector + PE32+ EFI Application header + EL2 → EL1 transition |
| `uts/aarch64/vectors.S`        | Exception vector table + KERNEL_ENTRY/EXIT macros                 |
| `uts/aarch64/trap.c`           | Trap dispatch, register dump, EL0-fault → SIGSEGV                 |
| `uts/aarch64/mmu.c`            | Identity-map page tables (44-bit PA), MMU enable, vmap_*          |
| `uts/aarch64/switch.S`         | `context_switch` with per-thread DAIF save/restore                |
| `uts/virt/timer.c`             | Generic timer @ 100 Hz                                            |
| `uts/virt/gic.c`               | GIC v3 distributor + redistributor + sysreg CPU interface         |
| `uts/virt/virtio_net.c`        | virtio-mmio network driver                                        |
| `uts/virt/efi_main.c`          | UEFI entry: ACPI 2.0 RSDP lookup + ExitBootServices               |
| `uts/virt/acpi.c`              | Walk RSDP → XSDT → MADT/MCFG/GTDT/FADT (AWS.md stage C)           |
| `uts/virt/pcie.c`              | ECAM bus enumeration; identifies AWS ENA + NVMe (AWS.md stage D)  |
| `uts/virt/nvme.c`              | NVMe 1.4 block driver (polled, 512 B LBAs) (AWS.md stage F)       |
| `uts/virt/ena.c`               | ENA network driver skeleton (AWS.md stage E, **byte-level constants unverified**) |
| `/home` on NVMe                | When a controller is present, `/home` is mounted off `nvme0n1` and persists across reboots (AWS.md stage F.1) |
| `tools/pad_pe.py`              | Pad `kernel.img` to PE SizeOfImage so EDK II accepts the load     |
| `uts/os/core/pmm.c`            | 4 KB-page freelist allocator (spinlocked for SMP)                 |
| `uts/os/core/kmem.c`           | Slab allocator + `kmalloc` size caches (lock order: kmem → pmm)   |
| `uts/os/proc/sched.c`          | Per-CPU dispatcher, wait queues, reap path                        |
| `uts/os/proc/signal.c`         | DEC/BSD reliable signals (sigaction, sigsuspend, sigprocmask)     |
| `uts/os/io/streams.c`          | mblk_t / dblk_t / queue_t / putq / getq / service procs           |
| `uts/os/io/stream_head.c`      | Stream head, drivers (loop/null/console/klog/fbcon), pipes        |
| `uts/os/io/cdevsw.c`           | SVR4 character-device switch keyed by major number                |
| `uts/os/io/tty.c`              | tty / ldterm line discipline                                      |
| `uts/os/fs/vfs.c`              | In-memory dentry/inode tree, fd table, vnode v_count              |
| `uts/os/fs/kfs.c`              | "kappara filesystem" — superblock + bitmap + dirent table         |
| `uts/os/fs/ramdisk.c`          | Block device backing kfs                                          |
| `uts/os/fs/procfs.c`           | `/proc/*` STREAMS chrdevs (see list below)                        |
| `uts/os/net/ipv4.c`            | IP multiplexor (`/dev/ip`), per-proto fan-out                     |
| `uts/os/net/{icmp,udp,tcp}.c`  | Pushable TPI modules above IP                                     |
| `uts/os/net/{lo,slip}.c`       | Stream drivers underneath IP (loopback, mini-UART SLIP)           |
| `uts/os/core/ftrace.c`         | Per-CPU function tracer (`make TRACE=1`)                          |
| `uts/os/proc/syscall.c`        | Syscall table + dispatcher                                        |
| `uts/os/core/kallsyms.c`       | Symbol-name lookup, frame-pointer backtrace                       |
| `uts/os/user/user.c`           | EL0 setup, `sys_execve`, per-thread user stacks                   |
| `uts/os/main.c`                | `kmain` orchestration                                             |
| `user/init.c`                  | Userspace shell (ksh) — runs at EL0                               |
| `user/syscall.h`               | User-side syscall numbers + inline asm wrappers                   |
| `cmd/`                         | `/usr/bin` programs: `ps`, `ping`, `ifconfig`, `netstat`, `test`, `nm`, `ldd`, `objdump`, ... |
| `lib/libc/`                    | Freestanding libc: crt0, printf, malloc, FILE*, string, io        |
| `lib/ld-kappara/`              | User-space dynamic linker (relocates DT_NEEDED + DT_HASH)         |
| `lib/libdltest/`               | Sample shared library exercised by `cmd/dltest`                   |
| `attic/raspi3b/`               | Retired Raspberry Pi 3 sources (kept for history)                 |
| `tools/gen_kallsyms.sh`        | nm + awk producing the symbol-table .S after pass-1 link          |
| `tools/kappara-cc.in`          | SDK wrapper compiler used by `smoke-sdk`                          |
| `docs/`                        | Reference documentation                                            |

### `/proc` entries

| File              | What it shows                                                  |
|-------------------|----------------------------------------------------------------|
| `/proc/ps`        | Threads: tid, state, priority, sched class, name               |
| `/proc/meminfo`   | pmm free pages + slab totals                                   |
| `/proc/slabinfo`  | Per-size-cache: name, obj_size, free / total                   |
| `/proc/streams`   | Registered STREAMS modules and drivers                         |
| `/proc/ftrace`    | Ring dump (read); `on`/`off`/`reset` (write)                   |
| `/proc/cpuload`   | Per-CPU load + idle ratio                                      |
| `/proc/netif`     | Registered network interfaces + IP/netmask                     |
| `/proc/slip`      | slip0 byte/frame counters                                      |
| `/proc/tcp`       | TCB table: state, ports, srtt, cwnd, ...                       |
| `/proc/acpi`      | ACPI RSDP / GIC bases / CPU MPIDRs / PCIe ECAM / timer GSIVs   |
| `/proc/pci`       | PCIe enumeration: BDF, vid:did, class, header, MSI-X cap, BARs |
| `/proc/efi`       | EFI memory map: type, physical_start, pages, end               |
| `/proc/nvme`      | NVMe controller summary: vid, model, sn, fw, ns1 size          |

The last four are populated only under the UEFI boot path; on
`-kernel` they print a one-line "not present" stub.

## Docs

| File                     | What's in it                                              |
|--------------------------|-----------------------------------------------------------|
| `docs/SHELL.md`          | Every `ksh` command, with examples                        |
| `docs/KED.md`            | The tiny ed-like editor                                   |
| `docs/VI.md`             | The modal vi-lite editor                                  |
| `docs/PROCFS.md`         | What every `/proc/*` and `/dev/*` exposes                 |
| `docs/ARCHITECTURE.md`   | Kernel internals: boot, MMU, scheduler, STREAMS, VFS, signals |
| `docs/BUILDING.md`       | Toolchain, build flags, run modes, QEMU quirks            |
| `docs/FTRACE.md`         | Per-CPU function tracer (`make TRACE=1`)                  |
| `docs/AWS.md`            | Staged path from QEMU virt to AWS EC2 Graviton via UEFI   |
| `docs/DYNAMIC.md`        | Dynamic linking roadmap (libc PIC → ld-kappara → dlopen)  |
| `docs/INDIE.md`          | "Run independently-built Linux-ABI binaries" roadmap      |

## License

Public domain / unlicensed.  Hack it up however you like.

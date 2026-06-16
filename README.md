# kappara

A small SVR4-flavored Unix-like operating system for AArch64 (Raspberry Pi 3
on QEMU today, eventually real Pi 4 hardware).  All four cores boot and
run an SVR4-style per-CPU dispatcher — each core has its own dispq, idle
thread, and `cpu_thread`; cross-CPU steal works.

The kernel uses SVR4 STREAMS for all character I/O — pipes, ttys, /dev/loop,
/dev/klog, /proc/*, the network stack — with module push/pop, queues, and
message blocks matching the AT&T conventions (mblk_t / qinit / streamtab).
Modules are discovered via `cdevsw[major]`.  Inode lifecycle follows vnode
`v_count` and `vop_inactive`.  Signals are reliable (DEC/BSD-style:
persistent handlers, sigmask, sigaction).  Numbering is POSIX (SIGTERM=15,
SIGKILL=9, ...).

Networking is a STREAMS multiplexor: `/dev/ip` is the mux, ICMP / UDP / TCP
are pushable modules above it, lo0 / slip0 are stream drivers underneath.
TCP is the RFC 793 full state graph (LISTEN through TIME_WAIT) with a
multi-accept backlog, RFC 6298 RTT estimation, exponential-backoff
retransmit, and real receive-window advertisement driven by STREAMS
backpressure.  See `docs/ARCHITECTURE.md` for the TCP phase table.

No soup for you.

## Quick start

Install cross toolchain + QEMU:

```
sudo apt-get install gcc-aarch64-linux-gnu qemu-system-aarch64
```

Build and run (headless, gentle on host CPU):

```
make run-thrifty
```

You'll get the `kappara:/#` shell.  Type `help` to see commands.

To quit:
- **QEMU stdio escape:** `Ctrl-A` then `x` — note that if you're in
  `screen` or `tmux`, those eat `Ctrl-A` by default; use `Ctrl-A a x`
  in screen, or the tmux prefix to escape it.
- **Last resort:** `make stop` from another terminal kills any running QEMU.

## Targets

| Target            | What it does                                                       |
|-------------------|--------------------------------------------------------------------|
| `make`            | Build `build/aarch64/kernel8.img`                                  |
| `make run`        | Boot under default QEMU args                                       |
| `make run-thrifty`| Boot with `-display none -accel tcg,thread=single` (caps host CPU) |
| `make run-gui`    | Boot with the splash window; needs working QEMU display backend    |
| `make stop`       | `pkill` any running QEMU process                                   |
| `make clean`      | Remove `build/`                                                    |

## What's inside

| File / dir                     | Role                                                              |
|--------------------------------|-------------------------------------------------------------------|
| `uts/aarch64/boot.S`          | Reset vector, EL3/EL2 → EL1 transition, secondary-core park       |
| `uts/aarch64/vectors.S`       | Exception vector table + KERNEL_ENTRY/EXIT macros                 |
| `uts/aarch64/trap.c`          | Trap dispatch, register dump, EL0-fault → SIGSEGV                 |
| `uts/aarch64/mmu.c`           | Identity-map page tables, MMU enable                              |
| `uts/aarch64/switch.S`        | `context_switch` with per-thread DAIF save/restore                |
| `uts/aarch64/timer.c`         | Generic timer @ 100 Hz                                            |
| `uts/aarch64/framebuffer.c`   | VC mailbox framebuffer + drawing primitives                       |
| `uts/aarch64/fbcon.c`         | Framebuffer text console (kprintf-tee off by default)             |
| `uts/aarch64/kallsyms_stub.S` | Pass-1 placeholder for the symbol-table link                      |
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
| `uts/os/fs/procfs.c`           | `/proc/{ps,meminfo,slabinfo,streams,ftrace,netif,slip,tcp,cpuload}` |
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
| `cmd/`                         | `/usr/bin` programs: `ps`, `ping`, `ifconfig`, `netstat`, `test`  |
| `lib/libc/`                    | Freestanding libc: crt0, printf, malloc, FILE*, string, io        |
| `tools/gen_kallsyms.sh`        | nm + awk producing the symbol-table .S after pass-1 link          |
| `docs/`                        | Reference documentation                                            |

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

## License

Public domain / unlicensed.  Hack it up however you like.

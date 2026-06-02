# kappara

A small SVR4-flavored Unix-like operating system for AArch64 (Raspberry Pi 3
on QEMU today, eventually real Pi 4 hardware).  All four cores boot —
core 0 runs the kernel + scheduler, cores 1-3 are released into a tiny
"hello + idle" entry through the standard ARM spin-table at PA 0xE0/E8/F0.
Real per-CPU scheduling is the next big step.

The kernel uses SVR4 STREAMS for character I/O — pipes, console, /dev/loop,
/dev/klog, /proc/* — with module push/pop, queues, and message blocks all
matching the AT&T conventions (mblk_t / qinit / streamtab).  Modules are
discovered via cdevsw[major].  Inode lifecycle follows vnode v_count and
vop_inactive.  Signals are reliable (DEC/BSD-style: persistent handlers,
sigmask).  Numbering is POSIX (SIGTERM=15, SIGKILL=9, etc.).

No soup for you.

## Quick start

Install cross toolchain + QEMU:

```
sudo apt-get install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabi qemu-system-arm
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
| `make ARCH=arm`   | ARMv7 target (bit-rotted; refuses with a pointer to what a revival needs) |

## What's inside

| File / dir                     | Role                                                              |
|--------------------------------|-------------------------------------------------------------------|
| `arch/aarch64/boot.S`          | Reset vector, EL3/EL2 → EL1 transition, secondary-core park       |
| `arch/aarch64/vectors.S`       | Exception vector table + KERNEL_ENTRY/EXIT macros                 |
| `arch/aarch64/trap.c`          | Trap dispatch, register dump, EL0-fault → SIGSEGV                 |
| `arch/aarch64/mmu.c`           | Identity-map page tables, MMU enable                              |
| `arch/aarch64/switch.S`        | `context_switch` with per-thread DAIF save/restore                |
| `arch/aarch64/timer.c`         | Generic timer @ 100 Hz                                            |
| `arch/aarch64/framebuffer.c`   | VC mailbox framebuffer + drawing primitives                       |
| `arch/aarch64/fbcon.c`         | Framebuffer text console (kprintf-tee off by default)             |
| `arch/aarch64/kallsyms_stub.S` | Pass-1 placeholder for the symbol-table link                      |
| `kernel/pmm.c`                 | 4 KB-page freelist allocator                                      |
| `kernel/kmem.c`                | Slab allocator + `kmalloc` size caches                            |
| `kernel/sched.c`               | Round-robin scheduler, wait queues, reap path                     |
| `kernel/signal.c`              | DEC/BSD reliable signals (fatal defaults today)                   |
| `kernel/streams.c`             | mblk_t / dblk_t / queue_t / putq / getq                           |
| `kernel/stream_head.c`         | Stream head, drivers (loop/null/console/klog/fbcon), pipes        |
| `kernel/cdevsw.c`              | SVR4 character-device switch keyed by major number                |
| `kernel/vfs.c`                 | In-memory dentry/inode tree, fd table, vnode v_count              |
| `kernel/kfs.c`                 | "kappara filesystem" — superblock + bitmap + dirent table         |
| `kernel/ramdisk.c`             | Block device backing kfs                                          |
| `kernel/proc.c`                | `/proc/{ps,meminfo,slabinfo,streams,ftrace}`                      |
| `kernel/ftrace.c`              | Per-CPU function tracer (`make TRACE=1`)                          |
| `kernel/syscall.c`             | Syscall table + dispatcher                                        |
| `kernel/kallsyms.c`            | Symbol-name lookup, frame-pointer backtrace                       |
| `kernel/user.c`                | EL0 setup, `sys_spawn` / `sys_exit`, per-thread user stacks       |
| `kernel/main.c`                | `kmain` orchestration                                             |
| `user/init.c`                  | Userspace shell (ksh) — runs at EL0 as PID 2                      |
| `user/syscall.h`               | User-side syscall numbers + inline asm wrappers                   |
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

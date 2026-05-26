# kappara

A small Unix-like operating system, written in C and a little assembly.
The long-term plan is a SVR4-style STREAMS framework for character I/O —
tty, pipes, network, and friends — sitting on top of a conventional
process / VFS kernel.

No soup for you.

## Status

Primary target: **AArch64** on QEMU `raspi3b` (Raspberry Pi 3 / BCM2837).
Boots at EL2, drops to EL1, enables the MMU with caches on, runs a slab
heap on a freelist physical page allocator, and round-robin-schedules
kernel threads under a 100 Hz generic-timer tick.

Secondary target: **ARMv7-A** on QEMU `-M virt -cpu cortex-a15`, chosen
because its Virt extensions match the MediaTek MT8125 (Cortex-A7).
Currently boots from HYP, drops to SVC, and prints over PL011 — the
arch-independent kernel code is not yet linked in for this target.

## Layout

```
arch/aarch64/   AArch64 boot, vectors, MMU, generic timer, context switch
arch/arm/       ARMv7 boot + PL011 UART (early bring-up)
kernel/         portable: printk, pmm, kmem (slab), sched, string, main
include/kappara/  shared headers
```

## Build & run

Needs cross-toolchains and QEMU:

```
sudo apt-get install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabi qemu-system-arm
```

AArch64 (default):

```
make                       # build/aarch64/kernel8.img
make run                   # qemu-system-aarch64 -M raspi3b ...
```

ARMv7:

```
make ARCH=arm              # build/arm/kernel-arm.img
make ARCH=arm run          # qemu-system-arm -M virt -cpu cortex-a15 ...
```

`make clean` removes the whole `build/` tree.  Quit QEMU with `Ctrl-A x`.

# kappara

A small Unix-like operating system for AArch64 (Raspberry Pi 3 / QEMU),
written in C and a little assembly. The long-term plan is a SVR4-style
STREAMS framework for character I/O — tty, pipes, network, and friends —
sitting on top of a conventional process / VFS kernel.

No soup for you.

## Status

Boots to EL2 on QEMU `raspi3b`, parks cores 1–3, and prints over PL011.
That's it so far. Next up: drop to EL1, enable the MMU, build a heap.

## Layout

```
arch/aarch64/   boot.S, linker.ld, PL011 UART
kernel/         kmain and (eventually) scheduler, vm, syscalls
```

## Build & run

Needs an AArch64 cross-toolchain and QEMU:

```
sudo apt-get install gcc-aarch64-linux-gnu qemu-system-arm
```

Then:

```
make            # produces build/kernel8.img
make run        # boots it in qemu-system-aarch64 -M raspi3b
make clean
```

Quit QEMU with `Ctrl-A x`.

Expected output:

```
kappara: hello from aarch64
        no soup for you, only streams
```

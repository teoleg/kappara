# Building and running kappara

## Host requirements

- Cross toolchain and QEMU:

  ```
  sudo apt-get install gcc-aarch64-linux-gnu qemu-system-aarch64
  ```

  Tested with Debian 12 toolchain (gcc-12).

- `make` (GNU make 4.x).

## Build

```
make                 # build/kernel.img (QEMU virt; the AWS Graviton on-ramp)
make TRACE=1         # build with gcc -finstrument-functions for ftrace
make clean
```

Single-arch since the raspi3b retirement -- Pi-specific drivers live
in `attic/raspi3b/`.  The kernel image targets QEMU `virt` today and
boots on AWS EC2 Graviton once `docs/AWS.md` stages A-G land.

### `TRACE=1` — function tracing

Passing `TRACE=1` turns on gcc's `-finstrument-functions` for the
whole kernel except a handful of TUs that sit on the tracer's own
path (`uts/os/ftrace.c`, `uts/os/printk.c`, `uts/aarch64/uart.c`,
`uts/os/string.c`, `uts/os/kallsyms.c`).  Each instrumented function
entry and exit becomes an event in a per-CPU ring buffer; `cat
/proc/ftrace` formats the buffer with symbol names + offsets.

This is a debug build — instrumentation forces de-inlining of
`static inline` helpers, slows the kernel materially, and is not
intended for the default build.  See `docs/FTRACE.md`.

The AArch64 build does a **two-pass link** to populate the symbol
table for backtraces — first link produces a temp ELF, `tools/gen_kallsyms.sh`
nm's it and emits a `.S` file with the sorted (address, name) table,
second link replaces the stub with the real table.  This is all
automatic; `make` Just Works.

### Header dependency tracking

`-MMD -MP` is on, and the `.d` files are `-include`'d at the bottom of
the Makefile.  Touching a header forces a rebuild of every `.o` that
includes it — no more silent ABI mismatches across stale .o files
(the bug from commit `3c2189b`).

## Run

| Target          | Behavior                                                      |
|-----------------|---------------------------------------------------------------|
| `make run`      | Boots `build/kernel.img` in QEMU `virt`, networking forwarded (FTP 2121, telnet 2323, FTP-PASV 30000-30007) |
| `make test`     | Full health check: smoke-ftp + smoke-sdk + smoke-linux + smoke-linux-mmap + `cmd/test all 14/14` |
| `make ami`      | Build a GPT+ESP raw disk image at `build/kappara-ami.img` (AWS.md stage G) -- requires `parted`, `dosfstools`, `mtools` |
| `make ami-run`  | Smoke-boot that image under QEMU + AAVMF -- requires `qemu-efi-aarch64` |
| `make stop`     | `pkill` any running QEMU                                      |
| `make run-telnet` | Boot headless + drive a foreground `nc localhost 2323` |
| `make run-ftp`  | Boot headless + an interactive `ftp 127.0.0.1 2121` session |

### Exercising the UEFI / AWS path

The same `build/kernel.img` is a PE32+ EFI Application (AWS.md
stages A-D); to actually take that path under QEMU you need
AAVMF as the firmware and an ESP partition holding the kernel
as `\EFI\BOOT\BOOTAA64.EFI`.  The scratch-pad script the agent
runs is the canonical recipe.  Once you're under AAVMF, drop in
extra PCIe devices to exercise stage D-F:

```
-device nvme,drive=nvm,serial=foo \
-drive id=nvm,format=raw,if=none,file=nvme.img
```

`nvme.img` can be any pre-created raw disk file; the driver
self-test in `nvme_init` writes a known pattern to LBA 0 and
reads it back, leaving the pattern persisted on the host file.

## Quitting QEMU

`make run` execs QEMU in the foreground with `-nographic`, which
multiplexes monitor + serial onto stdio:

| What you type                | What happens                                |
|------------------------------|---------------------------------------------|
| `Ctrl-A x`                   | QEMU `-nographic` escape -- exits cleanly   |
| `make stop` (other terminal) | `pkill -x` any running QEMU                 |

Note: `Ctrl-C` is interpreted by the kernel's tty as a SIGINT to
the foreground shell process (kappara's `init`), NOT as a quit
signal to QEMU.  If you really want a hard QEMU kill, use
`make stop` from another window.

### What changed from earlier versions

The original `-serial mon:stdio` mode reserved `Ctrl-A` for QEMU
monitor escapes (`Ctrl-A x` to quit, `Ctrl-A c` to enter the
monitor).  In `screen` and `tmux` Ctrl-A is normally the prefix
key, so it gets eaten before QEMU sees it, and the only way out was
killing QEMU from another window.  Dropping `mon:` removes monitor
access but gives us the much friendlier `Ctrl-C` exit and lets
`halt` work too.

## QEMU on a Raspberry Pi host

If you're running QEMU on a Pi 4 / 5 (aarch64 host emulating aarch64
guest), two things to know:

1. **Display window may segfault QEMU** (Mesa / GTK issue on
   Debian-on-Pi).  Use `-display none` — `make run-thrifty` does this.

2. **Host CPU spin**: QEMU TCG's WFI-thread-sleep doesn't always idle
   the per-vCPU host threads on the raspi3b machine.  The guest is
   doing the right thing (parked cores execute `wfi`), but QEMU
   itself keeps the host thread warm.  `-accel tcg,thread=single`
   collapses all 4 vCPUs to one host thread — `make run-thrifty`
   already passes this.  Add `taskset -c 0` if you want to cap the
   one remaining core too:

   ```
   taskset -c 0 make run-thrifty
   ```

   The proper fix is `-accel kvm` on a Pi 4 with kvm-arm enabled,
   which sidesteps TCG entirely.  That's an OS-install thing, not a
   kappara thing.

## Reading the boot log

Early boot prints over UART (PL011 on Pi 3 at 0x3F201000, captured by
QEMU's `-serial mon:stdio`).  Once the shell is up, kernel diagnostics
keep going to UART; the in-RAM ring buffer (`/dev/klog`) keeps a copy.

```
kappara:/# cat /dev/klog
... boot log ...
```

## Adding a new file to the build

Edit `Makefile`:

```
KERNEL_OBJS := \
    ...
    $(BUILD)/uts/os/your_new_file.o \
    ...
```

That's it — `-MMD -MP` picks up the header deps automatically.

## Adding a /usr/bin command

1. Create `cmd/<name>.c`.  It may include kernel ABI headers from
   `include/kappara/` (e.g., `icmp.h` for the `struct icmp_ping_req`
   layout) — `CMD_CFLAGS` already carries `-Iinclude`.
2. Add `<name>` to `CMD_NAMES` in `Makefile`.
3. Add an `.incbin` stanza to `uts/aarch64/usrblobs.S`.
4. Add `extern char <name>_blob_start/end[]` and a `PAY(...)` entry
   to `exec_space_init()` in `uts/os/user/user.c`.
5. Update `docs/SHELL.md` (the /usr/bin table).

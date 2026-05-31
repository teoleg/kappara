# Building and running kappara

## Host requirements

- Cross toolchains and QEMU:

  ```
  sudo apt-get install \
      gcc-aarch64-linux-gnu \
      gcc-arm-linux-gnueabi \
      qemu-system-arm
  ```

  Tested with Debian 12 toolchain (gcc-12).

- `make` (GNU make 4.x).  Builds the AArch64 image by default; ARM
  is currently link-broken (see TODO).

## Build

```
make                 # build/aarch64/kernel8.img
make ARCH=aarch64    # same, explicit
make ARCH=arm        # build/arm/kernel-arm.img (broken right now)
make clean
```

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
| `make run`      | Default args from each arch's `QEMU_ARGS`                     |
| `make run-thrifty` | `-display none -accel tcg,thread=single` — caps host CPU at one core |
| `make run-gui`  | With the splash window; needs working QEMU display backend    |
| `make stop`     | `pkill` any running QEMU                                      |

`make run-thrifty` is the right default on a Raspberry Pi host (or
any laptop where you don't want QEMU TCG to spin all cores).

## Quitting QEMU

`make run-thrifty` shares stdio with QEMU (monitor + serial both on
your terminal).  Standard escapes:

| Sequence       | What it does                                       |
|----------------|----------------------------------------------------|
| `Ctrl-A x`     | Quit QEMU                                          |
| `Ctrl-A c`     | Switch to QEMU monitor (then `q` to quit, `c` to resume) |
| `Ctrl-A h`     | Show all Ctrl-A escapes                            |

### When Ctrl-A doesn't work

If you're in `screen`, `tmux`, or some SSH wrappers, `Ctrl-A` is
intercepted before QEMU sees it.  Workarounds:

- **screen**: `Ctrl-A a x` — the first `Ctrl-A a` sends a literal
  `Ctrl-A` into the inner program (QEMU), and `x` is QEMU's quit.
- **tmux**: depending on prefix config, similar — escape your tmux
  prefix and send a raw Ctrl-A.
- **From another terminal**: `make stop` `pkill`s qemu cleanly.

`Ctrl-C` does **not** quit QEMU when stdio is shared; it gets
forwarded into the guest as a regular 0x03 byte.  The kappara shell
doesn't yet handle 0x03 as SIGINT (would need a controlling-terminal
concept), so it just appears in the line buffer.

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

## ARM target (broken)

`make ARCH=arm` builds object files but link fails because
`sys_spawn_impl` and `sys_exit_impl` are AArch64-only (defined in
`kernel/user.c`, which itself is aarch64-only).  Either stub them out
for ARM, or move them to a shared file with arch-specific helpers.
Not a current priority.

## Adding a new file to the build

Edit `Makefile`:

```
KERNEL_OBJS := \
    ...
    $(BUILD)/kernel/your_new_file.o \
    ...
```

That's it — `-MMD -MP` picks up the header deps automatically.

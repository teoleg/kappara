# Running Linux binaries on kappara

This is the roadmap for getting kappara from "binaries built
against our libc.so" to "any aarch64 Linux binary just runs."

Goal: zero SDK.  An external developer does

    aarch64-linux-musl-gcc hello.c -o hello

and `scp`s `hello` to a running kappara box, and `exec /home/hello`
prints "hello".  The binary thinks it's running on Linux.

The previous stage 1 (publishable kappara SDK + `kappara-cc`
wrapper) shipped as a sanity check; it stays in-tree as the
fallback for anyone who specifically wants to build against
kappara's own libc, but the main goal of this doc is now the
Linux-personality path.

## Why musl, not glibc

- musl is ~30× smaller than glibc and statically-linkable.
- musl makes no assumptions about `/proc`, `/sys`, NSS, locales,
  or any of the dozen surfaces glibc consults at startup.
- Static-musl binaries are completely self-contained ELFs --
  no `DT_NEEDED`, no `PT_INTERP`, no shared-library hunt.
- musl is the "what you'd ship inside a Docker scratch image"
  default for a reason.

glibc compat is theoretically possible but is a much bigger
investment for no real win at kappara's scale.

## Stages

### Stage 1 -- Run a Linux-ABI static-pie binary

Status: `[x]`

The minimum-viable Path B.  After this stage:

    aarch64-linux-gnu-gcc -static-pie -nostdlib -nostartfiles \
        -ffreestanding -fno-stack-protector -mgeneral-regs-only \
        -Wl,-e,_start -o hello hello.c

(where hello.c invokes Linux SYS_write=64 and SYS_exit=93 via raw
`svc #0`)

    scp hello kappara:/home/
    exec /home/hello

prints "hello from Linux-ABI static-pie on kappara".  No SDK, no
kappara-cc, no special flags, no kappara headers, no libc.

Full musl-libc support (printf, malloc, etc.) waits until stage
3 (mmap/mprotect) and stage 6 (musl-libc.so shipped at /lib/);
this stage is the proof-of-concept that the Linux syscall ABI
translates correctly.

What needs to land:

- **Linux syscall translation table.**  The arch trap dispatcher
  recognises both kappara's and Linux's `x8` syscall numbers and
  routes accordingly.  At minimum the dispatch covers what musl
  uses at startup (`SYS_mprotect`, `SYS_writev` or `SYS_write`,
  `SYS_exit_group`, `SYS_ioctl`, `SYS_set_tid_address` -- which we
  can fake).  Strategy: a separate `linux_to_kappara[]` array
  keyed on the Linux number; kappara natives keep their existing
  range.

- **Linux auxv entries on the exec stack.**  musl's `__init_libc`
  reads `AT_PAGESZ`, `AT_RANDOM`, `AT_HWCAP`, `AT_PLATFORM`,
  `AT_EXECFN`, `AT_SECURE`, `AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID`.
  The ones musl tolerates missing get omitted; the load-bearing
  ones (`AT_PAGESZ = 4096`, `AT_RANDOM = pointer to 16 bytes of
  noise on stack`) get populated.

- **`SYS_mmap`** (Linux 222).  Musl uses it for the malloc heap
  and signal-stack allocations.  Implementation maps anonymous
  zero-pages at a chosen VA in a new "anon" window (`ANON_VA =
  0x50000000`?) similar to how DLOPEN_VA works.

- **`SYS_mprotect`** (Linux 226).  Static-pie startup uses it
  for RELRO (mark pages read-only after relocs).  Initial impl
  can no-op (return 0) — we don't enforce W^X yet anyway.

- **`SYS_exit_group`** (Linux 94).  Same as our `SYS_exit`;
  just an alias.

What landed:

- New `linux_syscall_table[280]` in `uts/os/proc/syscall.c`.
  `syscall_dispatch` routes any `x8 >= 50` through it before
  falling through to the native table.  Today's entries:
  `ioctl`(29 — overlap-tolerated since the Linux-ABI binaries
  we ship today don't issue native `getpgrp`), `close`(57),
  `read`(63), `write`(64), `writev`(66), `exit`(93),
  `exit_group`(94), `set_tid_address`(96 — faked),
  `getpid`(172), `brk`(214), `mprotect`(226 — no-op for now).
- `linux_sys_writev` walks the iovec and issues `SYS_write`
  per entry -- the per-entry write isn't atomic but is enough
  for musl's startup printf path.
- `tools/sdk-test/linux-hello.c` -- proof binary using raw
  Linux syscall numbers via `svc #0`.  No libc, no SDK.
- `make ARCH=virt smoke-linux` -- end-to-end test: builds
  the binary with `aarch64-linux-gnu-gcc -static-pie`, FTP-
  uploads, exec's via telnet, greps for the expected message.

Verified: smoke-linux PASS; cmd/test all 14/14, smoke-ftp 2/3
(within noise); /bin/hello (ET_EXEC) still loads.

Limitations:

- The "Linux numbers >= 50 only" routing breaks any binary that
  uses `ioctl` (29) AND native `getpgrp` -- the latter doesn't
  exist for Linux-ABI binaries, so this is a non-issue today,
  but a real ABI flag should land before we trust this contract
  in mixed code.
- mprotect is a no-op.  RELRO is silently un-enforced.  Real
  W^X requires page-table flag flips.
- We don't yet handle Linux auxv conventions (AT_RANDOM,
  AT_PAGESZ, etc.).  Plain static-pie binaries don't read them;
  musl-linked ones will once stages 4-6 land.

### Stage 2 -- Bigger / flexible exec window

Status: `[x]`

`EXEC_VA = 0x20000000` with `EXEC_SIZE = 2 MB` is way too small.
A static-musl `hello` already pushes 30 KB; anything real wants
more.

- Bump `EXEC_SIZE` to 16 MB (we have plenty of VA space, the
  vm_map's L2 covers 1 GB).
- Allow PT_LOADs with arbitrary `p_vaddr` -- not just our
  linker-script-assumed 0.  Pick `load_base` based on the lowest
  `p_vaddr` and shift everything by the same delta.
- Validate the relocated VA range stays inside `[EXEC_VA,
  EXEC_VA + EXEC_SIZE)` -- same as today, just bigger.

After this stage anything up to ~16 MB of code+data works.

What landed:

- `EXEC_SIZE` bumped 2 MB -> 16 MB.  `EXEC_STACK_VA` shifted to
  `0x21000000`, `EXEC_HEAP_VA` to `0x21200000`.  New
  `EXEC_STACK_SIZE = 2 MB` constant; `EXEC_HEAP_SIZE` bumped to
  4 MB.
- `EXEC_TOTAL` in uaccess.c updated to span 22 MB
  (code + stack + heap).
- **Bug surfaced and fixed in mmu.c**: fresh vm_maps inherit the
  kernel's identity-mapped L2 BLOCKs (`vmap_dup_kernel_l2`).
  `mmu_vmap_user_va_to_kva` used to return the kernel KVA for any
  L2 BLOCK entry, which made `fork_clone_pages` think 16 MB of
  EXEC_VA was full of valid user pages and try to deep-clone
  kernel memory.  Pre-stage-2 EXEC_VA fit in one 2 MB L2 chunk
  that was always TABLE-promoted by the first vmap_alloc_pages,
  so the bug was masked.  Fix: `mmu_vmap_user_va_to_kva` now
  returns NULL for L2 BLOCK entries -- user pages always go
  through TABLE+L3.

### Stage 3 -- `SYS_mmap` / `SYS_munmap` / `SYS_mprotect`

Status: `[x]`

What landed:

- `ANON_VA = 0x22000000`, `ANON_SIZE = 128 MB` (kept inside the
  1 GB L2 range, after EXEC's 22 MB total).
- `struct vm_map` gains `anon_next` -- high-water mark that bumps
  by every mmap request.  Initialised to `ANON_VA` on first use.
- `sys_mmap_impl(addr, length, prot, flags, fd, offset)`: rejects
  anything without `MAP_ANONYMOUS`, rounds length up to page,
  allocates pages, maps from `anon_next`, bumps it.  Address-hint
  is ignored (we always pick).
- `sys_munmap_impl(addr, length)`: walks the range and unmaps.
  Doesn't reclaim the VA range -- the high-water mark only grows.
  Real free-range tracking is a future refinement.
- `sys_mprotect_impl`: stub returning 0 -- enough to satisfy
  static-pie RELRO startup.  Real W^X requires AP-bit flips on
  existing L3 entries, deferred.
- Linux dispatch: `mmap=222`, `munmap=215`, `mprotect=226` route
  through the new impls.
- `fork()` clones the ANON range and inherits `anon_next` so
  mmap'd pages survive across fork.
- `tools/sdk-test/linux-mmap.c` -- proof: SYS_mmap then write a
  magic string then SYS_write the string back via the new page
  then SYS_munmap then SYS_exit.  Verified by
  `make ARCH=virt smoke-linux-mmap`.

Verified: smoke-linux PASS, smoke-linux-mmap PASS, cmd/test all
14/14, smoke-ftp 3/3.

### Stage 4 -- `PT_INTERP` for dynamic musl binaries

Status: `[ ]`

Once stages 1-3 handle static-pie, the dynamic case becomes
straightforward.  `aarch64-linux-musl-gcc hello.c -o hello`
(no `-static-pie`) produces a binary with
`PT_INTERP = /lib/ld-musl-aarch64.so.1`.

- Kernel `sys_execve_impl` reads `PT_INTERP`.  If present, opens
  that file via VFS and loads it as the entry point.
- The kernel-shipped `ld-kappara.so` stays the loader for our own
  cmd binaries (no `PT_INTERP`).
- A symlink (`vfs_link`?) or a parallel registration at
  `/lib/ld-musl-aarch64.so.1` → musl's ld.so binary.

### Stage 5 -- General `DT_NEEDED` resolution

Status: `[ ]`

For a dynamic musl binary to actually run we need a real ld.so
that walks `DT_NEEDED`.  Two options:

- A. **Use musl's own ld.so.**  Ship `ld-musl-aarch64.so.1`
  (the real one, from upstream musl) at `/lib/`.  It handles
  `DT_NEEDED` resolution natively; we just point `PT_INTERP` at
  it.  Probably the right call -- we don't need to reimplement
  ld.so semantics.
- B. **Teach ld-kappara.so DT_NEEDED.**  Extend our existing
  user-space linker to walk `DT_NEEDED`, mmap each `.so` from
  `/lib/`, and merge symbol tables.  Smaller artefact, but more
  code in our tree.

Decision deferred to whichever feels less painful at the time.

### Stage 6 -- Ship a musl-libc.so at `/lib/`

Status: `[ ]`

Build musl-libc-aarch64 once, on the host, ship the resulting
`libc.musl-aarch64.so.1` as a kappara blob (same incbin trick as
our libc.so / dltest.so today).  Register it at the path the
musl PT_INTERP expects.

Once this lands, *dynamic* musl binaries run unmodified.  This is
the moment Path B becomes "real Linux binaries".

### Stage 7 -- Port a few real programs

Status: `[ ]`

Pick a ladder and walk it.  Each port surfaces a syscall musl
uses that we hadn't seen yet; add it to the translation table.

- `sqlite3 :memory:` (no FS needed)
- `lua` REPL
- a busybox subset (`ls`, `cat`, `wc`)
- a small static webserver (e.g. `mongoose` single-file)

At each step: build the program on host with
`aarch64-linux-musl-gcc -static-pie`, scp it, run it, fix what
breaks.

## What this is NOT

- A goal to run glibc binaries.  Musl-only.
- A goal to run x86_64 binaries.  Aarch64-only.
- A goal to implement every Linux syscall.  Only the ones musl
  + the ported programs actually issue.  Realistically that's
  ~40-60 syscalls, not all 400+.
- A goal to be `binfmt_misc`-compatible or to support multiple
  personalities concurrently.  One ABI translation, one direction.

## What about the SDK?

Stage 1 of the previous INDIE plan shipped a working SDK +
`kappara-cc`.  That still exists -- the build targets (`make
sdk`, `make sdk-tarball`, `make ARCH=virt smoke-sdk`) stay in
the Makefile and the tarball still ships.  It's just no longer
*the* path -- it's the fallback for "I specifically want to
build against kappara, not against Linux."

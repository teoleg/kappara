# Running independently-built software on kappara

This is the roadmap for getting kappara from "every binary is
checked into the source tree" to "anyone can build a binary
elsewhere, drop it on a running kappara box, and have it just
work."

Goal: a clean line where the kappara source tree and the
*things that run on kappara* are two separate worlds.  External
developers should not need our `cmd/`, our `Makefile`, our
`/usr/bin` blob registration, or any commit access to ship code
for kappara.

The shape of the deliverable: an SDK tarball + a `kappara-cc`
wrapper.  `kappara-cc hello.c -o hello && scp hello kappara:/home/`
and it runs.  No special blessing required.

Stages, mirroring the convention from `docs/FTPD.md` /
`docs/DYNAMIC.md`: each one flips `[ ]` to `[x]` when its work
lands.

## What we already have (DYNAMIC.md stages 1-8)

- Real ELF dynamic linker in user space (`lib/ld-kappara`).
- `libc.so` shared.  Cmd binaries link against it via DT_NEEDED.
- `dlopen` / `dlsym` / `dlclose` / `dlerror` syscall surface.
- PIE binaries load at `EXEC_VA = 0x20000000`, libc.so at
  `LIBC_VA = 0x38000000`, ld.so at `LD_VA = 0x30000000`.
- ELF inspection tools in `/usr/bin`: `nm`, `ldd`, `objdump`.

The dynamic-linking machinery is in place; what's missing is the
SDK packaging, a few loader features (PT_INTERP, general
DT_NEEDED), a bigger code window, and enough libc surface for
non-toy programs.

## Stages

### Stage 1 -- Publishable SDK (`kappara-cc`)

Status: `[x]`

Ship a tarball that contains everything a foreign toolchain needs:

- `sysroot/include/`   -- `<stdio.h>`, `<stdlib.h>`, `<string.h>`,
                          and the public `kappara/abi/*.h` headers.
- `sysroot/lib/libc.so` -- the same shared object the in-tree
                           build produces.
- `sysroot/lib/crt0.o`  -- `_start` for cmd binaries.
- `sysroot/lib/prog_linker.ld` -- our PIE linker script.
- `sysroot/lib/ld-kappara.so` -- bundled so external builds can
                                 reference the same loader.

And a `bin/kappara-cc` wrapper script that invokes
`aarch64-linux-gnu-gcc` with:

    --sysroot=<sdk-root>
    -ffreestanding -nostdlib -nostartfiles
    -fno-stack-protector -fPIE -mgeneral-regs-only
    + linker glue (`-pie --no-dynamic-linker -l:libc.so` etc.)

Test deliverable: `tools/sdk-test/hello.c` (a `printf("hi")`
binary) builds via `kappara-cc` from a clean directory (no
checkout of `uts/`), uploads via FTP, runs.

What landed:

- `build/sdk/sysroot/{include,lib}` -- staged sysroot with libc
  headers, kernel ABI headers, libc.so, crt0.o, prog_linker.ld,
  ld-kappara.so.
- `tools/kappara-cc.in` -- shell wrapper that invokes
  `aarch64-linux-gnu-gcc` with `--sysroot=...` and the right
  -fPIE/-pie flags + linker script + DT_NEEDED libc.so.
- `make sdk` -- assembles the SDK.
- `make sdk-tarball` -- packages it as `build/kappara-sdk.tar.gz`.
- `make ARCH=virt smoke-sdk` -- end-to-end test: builds
  `tools/sdk-test/hello.c` in a fresh `/tmp` dir using only the
  SDK (no access to `uts/`), boots kappara virt, FTP-uploads the
  resulting binary, exec's it via telnet, checks output.

Verified: smoke-sdk passes 3/3.  An external developer can
extract `kappara-sdk.tar.gz`, run
`kappara-sdk/bin/kappara-cc hello.c -o hello`, FTP it to a
running kappara instance, and exec it.

### Stage 2 -- Honour `PT_INTERP`

Status: `[ ]`

Today the kernel hardcodes the loader path: every `ET_DYN`
exec gets `ld-kappara.so` whether the binary asks for it or
not.  Real-world binaries embed their interpreter as a
`PT_INTERP` segment (e.g. `/lib/ld-kappara.so`).

What changes:

- Drop `--no-dynamic-linker` from `kappara-cc`'s default flags;
  externally-built binaries will carry `PT_INTERP = /lib/ld-kappara.so`.
- Kernel `sys_execve_impl` reads `PT_INTERP`, verifies it
  matches a permitted loader, loads THAT (not the hardcoded one).
- For backward compatibility: binaries without `PT_INTERP` (our
  current cmd ELFs) still get `ld-kappara.so` at `LD_VA`.

This is a small kernel patch (~30 lines).  Mostly an honesty fix:
we should respect what the binary says.

### Stage 3 -- General `DT_NEEDED` resolution in ld.so

Status: `[ ]`

Today ld.so only knows about `libc.so` because the kernel pre-
loaded it at a known VA and passed `AT_KAPPARA_LIBC_BASE`.  A
binary that `DT_NEEDED`s a second library (`libdltest.so`,
`libssl.so`, whatever) has no way to get it loaded at exec time.

What needs to land:

- New syscall `SYS_mmap_file` (or extend dlopen so ld.so can
  call it): open a VFS path, load its PT_LOADs at a chosen VA,
  return the load base.  Same primitive dlopen uses today.
- ld.so walks the app's `.dynamic` for `DT_NEEDED` strings.
- For each name, search `/lib/` (and maybe `/usr/lib/`) for a
  matching file, call `SYS_mmap_file`, recurse into the loaded
  object's own `DT_NEEDED` chain.
- Build a merged symbol table from all loaded objects; resolve
  `GLOB_DAT` / `JUMP_SLOT` against it (currently we only look in
  libc.so).

After this stage, a binary that does
`DT_NEEDED [libsomething.so]` just works as long as
`/lib/libsomething.so` exists.

### Stage 4 -- Bigger code window + variable `load_base`

Status: `[ ]`

`EXEC_VA = 0x20000000` is only a 2 MB window.  Real binaries
push past this fast (any C++ program with even modest STL use
is bigger than that).

What changes:

- Bump `EXEC_SIZE` from 2 MB to 16 MB (or 64 MB).
- Each `vm_map`'s L2 table covers 1 GB; we have plenty of room.
- Pick `load_base` based on the binary's `PT_LOAD` layout
  rather than hardcoding `EXEC_VA`.  Today we assume p_vaddr=0
  for the first PT_LOAD; allow others (binaries linked at
  0x400000 etc.).

Optional: ASLR-style placement.  Not a priority for hobby OS.

### Stage 5 -- libc surface expansion (musl-subset)

Status: `[ ]`

Our libc has ~70 functions.  Real software uses hundreds.  Pick
musl as the reference (much smaller and cleaner than glibc) and
implement the functions an "average" C program uses.

Likely big wins:

- `<stdio.h>`: `printf` family is mostly OK; need `vfprintf`
  exposing more format specifiers (`%a`, `%g`, `%n` — debatable).
- `<unistd.h>`: `getopt`, `sleep` (need `SYS_clock_nanosleep`),
  `usleep`, `getcwd`, `chdir`.
- `<sys/time.h>`: `gettimeofday`, `clock`.
- `<sys/stat.h>`: `stat`, `fstat`, `mkdir` (we have).
- `<string.h>`: `strerror`, `strcasecmp`, `strspn`, `strcspn`.
- `<stdlib.h>`: `setenv`/`getenv` (need a real env vector),
  `system`/`popen` (hard -- needs `posix_spawn`).
- `<pthread.h>`: significant.  Threads exist via `sys_spawn` --
  need a libc layer that mimics pthreads against it.

Defer: locales, wide chars, full POSIX regex, networking
beyond what STREAMS gives us.

### Stage 6 -- Port real third-party software

Status: `[ ]`

Pick three programs of increasing complexity, build them
externally via `kappara-cc`, run them on kappara.  Each one
exposes gaps that get fixed in stage 5.

Suggested ladder:

1. **`sqlite3`** -- single .c file, statically linked, almost
   pure C.  Good first target.
2. **`lua`** -- two .c files for the interpreter + a separate
   small program.  Tests dlopen for the C API.
3. **`coreutils`'s `ls`** -- exercises getopt, readdir
   (we don't have `getdents` yet), stat, etc.  Compare output
   against our built-in `/usr/bin/ls`.

Each port is its own change set; the SDK is what makes the work
possible.

### Stage 7 -- (Stretch) Linux personality layer

Status: `[ ]` (optional)

Run *unmodified Linux aarch64 ELFs* by:

- Reading `PT_INTERP = /lib64/ld-linux-aarch64.so.1` and
  redirecting to a Linux-compat ld-kappara loader.
- Loading musl-libc.so (built once on host) instead of our
  libc.so.
- A syscall translation table at EL1: when an EL0 binary issues
  `svc #0` with `x8 = 64` (Linux write), the kernel sees the
  Linux number, looks up the equivalent kappara handler (`SYS_write`
  = 6), and calls it.
- Mostly a "match enough syscalls" exercise; the syscall set
  Linux apps actually use day-to-day is ~50, not the full ~400.

This is the biggest single-stage lift in the plan and the most
optional.  Don't start until 1-6 are solid.

## Dependency chain

```
1. SDK -----+
            |
            v
2. PT_INTERP
   ld.so
            |
            +-> 3. General DT_NEEDED
            |
            +-> 4. Bigger EXEC window
            |
            +-> 5. libc expansion <--+
                                     |
                                     |
            6. port real software ---+
                                     |
            7. Linux personality (stretch)
```

Stages 2-4 are largely independent and small; 5 is the biggest
ongoing investment.  6 drives 5 by surfacing real gaps.

## What this is NOT

- A goal to run macOS / Windows / BSD binaries.  AArch64 Linux
  ELFs only, when we get there.
- A goal to be a glibc-binary-compatible target.  Musl's surface
  is what we'll aim at for stretch goal.
- A goal to ship a distribution.  We're building a runtime; what
  people ship on top is theirs.

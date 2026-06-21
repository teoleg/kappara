# Dynamic linking plan -- real libc, real linker, .so support

This is the roadmap for getting kappara from "every binary statically
links a thin libc" to "binaries are PIE, share a real `libc.so` mapped
once, and the kernel hands control to a dynamic loader via PT_INTERP".

Same structure as `docs/FTPD.md`: each stage carries a `Status:`
line that flips from `[ ]` to `[x]` when the work lands.  The
prerequisite chain matters -- skipping ahead is how you end up
debugging relocations in a kernel that can't apply them.

## What we have today

- **libc**: ~25 functions across `lib/libc/src/{string,printf,stdlib,
  io,signal,malloc,file}.c`.  Real-binary-shaped surface but lots of
  gaps (no ctype, no `errno`, no `setjmp`, no `time`).
- **Static link only**: `cmd/<x>.elf` is `crt0.o + <x>.o + libc.a`,
  linked with `user/prog_linker.ld` at the fixed VA `EXEC_VA =
  0x20000000`.  Each binary ships its own `printf`.  Smallest ELF is
  ~12 KB and most of it is libc.
- **No dynamic loader, no relocations at load time, no PIE/PIC, no
  shared objects, no `dlopen`.**

## Architecture choices (locked)

These constrain everything below.

- **AArch64 only, ELF64 only.**  We have no plans to support 32-bit
  or non-ELF formats; the relocation types we have to handle are
  the standard AArch64 ABI set: `R_AARCH64_RELATIVE`,
  `R_AARCH64_GLOB_DAT`, `R_AARCH64_JUMP_SLOT`,
  `R_AARCH64_TLSDESC` (deferred -- no thread-local storage yet).

- **One shared object: `libc.so`.**  At least to start.  Every
  binary `DT_NEEDED`s `libc.so`; the kernel maps it once into the
  process's address space.

- **One known dynamic linker path: `/lib/ld-kappara.so`.**  The
  kernel pokes this into `PT_INTERP` itself at execve time, not by
  reading the ELF header (which is how POSIX does it).  Simplifies
  the loader's bootstrap: no environment, no AT_PHDR vector
  parsing in stage 1.

- **Lazy PLT resolution is optional.**  Stage 5's loader resolves
  all relocations eagerly first.  Once that works we can decide
  whether to wire the resolver trampoline.

## Stages

### Stage 1 -- libc fill-out

Status: `[x]`

Why first: real binaries built on real libc lean on functions we
don't have.  Adding them now means stages 2+ aren't tripped by
"oh, this stub doesn't exist yet".

What landed:

| Header     | Functions                                                          |
|------------|--------------------------------------------------------------------|
| `<string.h>` | `strrchr`, `strdup`, `strncat`, `strstr`, `strtok`, `memchr` (`strncpy`, `memmove` already shipped) |
| `<stdlib.h>` | `qsort` (insertion sort), `bsearch`, `abs`, `labs`, `getenv` (stub: returns NULL) |
| `<ctype.h>`  | `isdigit`, `isalpha`, `isalnum`, `isspace`, `isupper`, `islower`, `isxdigit`, `isprint`, `iscntrl`, `ispunct`, `toupper`, `tolower` |
| `<errno.h>`  | `int errno;` (global -- TLS deferred) + 35 standard `E*` constants |
| `<stdio.h>`  | `perror`, `fflush` (no-op), `getchar`, `getline` (malloc-grow) |
| `<setjmp.h>` | `setjmp` / `longjmp` in aarch64 asm; saves x19-x30 + SP.  d8-d15 deliberately omitted because userland builds with `-mgeneral-regs-only` and CPACR_EL1.FPEN doesn't grant EL0 FP access |
| `<time.h>`   | `clock_gettime(CLOCK_MONOTONIC, ts)` + `time(NULL)` via new `SYS_clock_gettime` (35), kernel reads CNTPCT_EL0 / CNTFRQ_EL0 |

Not done: `atof`.  `-mgeneral-regs-only` rejects `double` return
types; we can't legally declare it under the current ABI.  Once
FPEN is enabled (out of scope) the prototype can be added.

Build artefacts: added `lib/libc/src/{ctype,errno,time}.c`,
`lib/libc/include/{ctype,errno,setjmp,time}.h`, and
`lib/libc/aarch64/setjmp.S`.  Makefile's LIBC_SRCS extended; new
`$(LIBC_SETJMP)` rule mirrors crt0.

Side dependencies: bumped `KFS_NAME_MAX` 16→12 + `KFS_DIRENTS`
18→21 to fit a 19th /usr/bin entry (`uptime`) which exercises the
new `clock_gettime` syscall + `<time.h>` end-to-end.

Test: `cmd/test all` 13/13, raspi3b boots to prompt, `uptime`
prints "up Ns".  `smoke-ftp` flake rate unchanged (2/5 on this
branch and on pristine HEAD -- pre-existing TCP race, unrelated
to libc growth).

### Stage 2 -- Convert libc to position-independent code

Status: `[x]`

What changed: `LIBC_CFLAGS` swapped `-fno-pie -fno-pic` for `-fPIC`.
libc's `.o` files now use only PC-relative relocations
(`R_AARCH64_CALL26`, `R_AARCH64_ADR_PREL_PG_HI21`,
`R_AARCH64_LDST64_LO12`, `R_AARCH64_PREL32`) inside `.text` —
the previous `R_AARCH64_ABS64` entries are gone except where they
belong (`.eh_frame`, `.debug_*`, both runtime-irrelevant).

What this does NOT do yet: produce a `.so`.  Stage 2's output is
still `libc.a`, just with PIC-compatible code; the existing static
link path stays working.  We've just stopped relying on link-time
address fixups in libc.

Observed side-effect: cmd binaries shrank by ~64 bytes each (PIC
encodings happen to be one instruction shorter for our access
patterns), pushing `ifconfig.elf` out of the size band that
exposes the pre-existing TCP send-buffer race -- smoke-ftp now
PASS 5/5 instead of the 2/5 baseline.  The race is still there
in tcp.c; we just stopped hitting it in this test rig.

Test: `cmd/test all` 13/13, `make ARCH=virt smoke-ftp` PASS 5/5.

### Stage 3 -- Convert cmd binaries to PIE

Status: `[ ]`

What changes: `CMD_CFLAGS` gets `-fPIE`, `cmd/*.elf` linker line
gets `-pie`, and `user/prog_linker.ld` becomes either empty or
just sets `OUTPUT_FORMAT(elf64-littleaarch64)` and the page size.
The linker produces ET_DYN binaries (with a notional load address
of 0) and a `.dynamic` section even for "statically linked" PIE
output.

What this DOESN'T do yet: actually use the loader for relocations.
The exec loader at this point still memcopies `PT_LOAD` and assumes
the binary is happy at its preferred address.  PIE binaries
preferring 0 means we map them at EXEC_VA = 0x20000000, and
**relocations referencing globals will be wrong** until stage 4
applies them.

What we get: confirmation that the build pipeline produces
ET_DYN.  Stage 3's deliverable is the change in `.dynamic` and a
build that still produces working binaries because their PIE
relocations happen to coincide with the offsets the loader picks
(this works for `cmd/hello.c`-shaped binaries; non-trivial ones
break, which is the motivation for stage 4).

### Stage 4 -- Apply ELF relocations at load time

Status: `[ ]`

This is the keystone.  Without it nothing past here works.

What to build: in `uts/os/user/user.c`, when execve loads an ELF
with `PT_DYNAMIC`, walk the dynamic tags, locate `.rela.dyn` /
`.rela.plt` / symbol table / string table, and apply every
`R_AARCH64_RELATIVE` (load_base + addend), `R_AARCH64_GLOB_DAT`
(look up symbol; for stage 4 every symbol resolves inside the
binary itself), and `R_AARCH64_JUMP_SLOT` (eager resolution for
now).

The exec loader keeps loading at EXEC_VA; what's new is the
fixup walk after `PT_LOAD` copies finish.  This is the moment
"PIE binary that references a global" starts actually working.

Test: write a PIE test binary that does `static int x = 42;`
read by an `extern` function later in the same .so -- specifically
exercises `GLOB_DAT`.  `cmd/test all` 13/13 still.

### Stage 5 -- Dynamic linker: `/lib/ld-kappara.so`

Status: `[ ]`

A user-space program that the kernel hands control to instead of
the application's `_start`.  Bootstrap shape:

1. Linker is itself a fully self-contained, statically-linked ELF
   (no DT_NEEDED on itself).  It's mapped into the process at
   load-time by the kernel; the kernel sets the entry point in
   the trap frame to the linker's `_start`, not the application's.
2. Linker reads the application's `.dynamic` from `PT_DYNAMIC`,
   processes `DT_NEEDED` entries, mmaps each `.so` (only
   `libc.so` to start), applies the same relocation walk stage 4
   does -- but against the merged symbol table from every loaded
   object.
3. After relocations, linker jumps to the application's `_start`.

The kernel-side change is: on execve of a binary with `PT_INTERP`
set (or always, in our case -- we force `PT_INTERP =
"/lib/ld-kappara.so"`), load BOTH the linker and the application,
hand control to the linker.

`auxv` (a small array of (type, value) tuples) tells the linker
where the application's PT_PHDR is, what the page size is, etc.
We pass it as a third argument to `_start` past argv / envp; the
crt0.S for ld-kappara unpacks it.

Test: build a "hello world" PIE app that DT_NEEDED's libc.so,
upload via FTP, exec.  Should print "hello, world".

### Stage 6 -- Convert libc to `libc.so`

Status: `[ ]`

What changes: `LIBC` builds as a shared object (`-shared`, with a
proper SONAME), gets registered as `/lib/libc.so` via the same
`vfs_mknod_regfile` path /bin uses today.  cmd binaries link
against the shared object (`-l:libc.so`) instead of `libc.a`;
every binary now ships with a `DT_NEEDED libc.so` entry.

What we get: each cmd binary shrinks by ~10 KB (no per-binary
copy of printf).  More importantly, the dynamic-linking pipeline
is fully exercised.

Test: `smoke-ftp PASS`, binary sizes shrink visibly.

### Stage 7 -- `dlopen` / `dlsym`

Status: `[ ]` (optional)

User-space wrappers around the linker's "load this .so" path.
Useful for FTP-uploading new modules and loading them at
runtime without re-execve.  Wait until 1-6 are solid.

## Open questions

- **Where does `ld-kappara.so` live?**  Currently `/bin` is blob
  files registered at boot; `/lib` doesn't exist.  Decide whether
  to create `/lib` as a new blob-mounted directory or expand `/bin`'s
  scope.
- **PLT lazy resolution**: skipped initially.  Wire the trampoline
  if any later workload needs it.
- **TLS** (`R_AARCH64_TLSDESC`, `__tls_get_addr`): completely
  deferred.  Real-world libc uses TLS for errno per-thread; we
  cheat with a single global `errno` for now.
- **fork after dlopen**: the child inherits the loaded .so map but
  not the linker's state.  Easy bug to step on; document it once
  stage 5 lands.

## What this is NOT

- A POSIX-conformant `ld.so`.
- An attempt to load Linux ELFs.  Kappara's syscall ABI is its
  own; nothing built for Linux will load here.
- A general dynamic linker for any architecture.  Aarch64-only,
  ELF64-only, kappara-only.

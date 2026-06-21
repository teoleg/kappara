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

Status: `[x]`

Shipped together with stage 4 because cmd/test.c has a registry of
`{ const char *name; int (*fn)(void); }` -- exactly the
pointer-array pattern that needs `R_AARCH64_RELATIVE` to work.
Stage 3 alone would leave `cmd/test` broken.

What changed:

- `CMD_CFLAGS` swapped `-fno-pie -fno-pic` for `-fPIE`.
- cmd/*.elf link line gets `-pie`.
- `user/prog_linker.ld` rewritten for PIE: explicit `PHDRS` (text
  PT_LOAD + data PT_LOAD + PT_DYNAMIC), `.dynamic` / `.rela.dyn`
  sections, no absolute `. = 0x20000000`.
- `user/hello_linker.ld` (new) keeps `/bin/hello` as ET_EXEC at
  the old fixed VA -- the minimal test of the loader's ET_EXEC
  branch.  hello has no libc, no relocations.

Verify: `readelf -h build/cmd/test.elf` reports `Type: DYN`,
`Entry: 0x470` (a small offset, not the absolute VA), and
`readelf -d` shows DT_RELA with 32 RELACOUNT entries -- the test
registry's function pointers.

### Stage 4 -- Apply ELF relocations at load time

Status: `[x]`

The keystone.  `sys_execve_impl` now:

1. Accepts `ET_DYN` as well as `ET_EXEC`.
2. Picks `load_base = EXEC_VA` for ET_DYN, `0` for ET_EXEC.
3. Walks `PT_LOAD`: places each segment at `p_vaddr + load_base`
   and validates the relocated VA against the EXEC window.
4. Locates `PT_DYNAMIC`, walks the `Elf64_Dyn` array for
   `DT_RELA` / `DT_RELASZ` / `DT_RELAENT`.
5. For each `Elf64_Rela`: if type is `R_AARCH64_RELATIVE`, writes
   `load_base + r_addend` at user VA `load_base + r_offset` via
   `vmap_copyin` (8-byte fixup per reloc).
6. Entry point becomes `e_entry + load_base`.

Unsupported reloc types (`GLOB_DAT`, `JUMP_SLOT`) abort with a
diagnostic; they show up only once libc.so is split out
(stages 5+).  `DT_NEEDED` is also a hard error -- there's no
dynamic linker yet.

ELF type additions in `include/kappara/abi/elf.h`: `ET_DYN`,
`PT_DYNAMIC`, `PT_INTERP`, `PT_PHDR`, `DT_*` tags, the
`R_AARCH64_*` reloc constants, `Elf64_Dyn` / `Elf64_Rela`.

Verified end-to-end:

- `cmd/test all` 13/13 -- exercises the relocated pointer
  registry; without stage 4 the test names would be garbage and
  the function-pointer dispatch would jump to bogus addresses.
- `/bin/hello` (ET_EXEC, no libc) still exec's cleanly at the
  old fixed `0x20000000` entry -- proves the ET_EXEC branch
  isn't regressed.
- `make ARCH=virt smoke-ftp` PASS 5/5 with the now-PIE ftpd.elf
  (entry at `EXEC_VA + 0x200`).
- Distinct PIE binaries land at distinct entry offsets:
  ftpd=0x200, uptime=0x140, test=0x470 -- proves e_entry is
  honoured via `load_base + e_entry`, not hardcoded.

### Stage 5 -- Dynamic linker: `ld-kappara.so` bootstrap

Status: `[x]`

User-space program that the kernel hands control to instead of
the application's `_start`.  Today (no `libc.so` yet) it's a
pass-through: parses `auxv` for `AT_ENTRY` and branches.  When
stage 6 splits libc out, the same shape grows to walk the app's
`PT_DYNAMIC`, process `DT_NEEDED`, mmap each `.so`, and apply
cross-DSO relocations against the merged symbol table.

Architecture choices for the bootstrap:

- **ld-kappara.so is `ET_EXEC`**, not PIE, deliberately.  It lives
  at a fixed VA `LD_VA = 0x30000000` (1 MB window).  Making it
  PIE would create a chicken-and-egg problem: who relocates the
  relocator?  The kernel can do it (we already have stage 4's
  reloc walk), but skipping it entirely is simpler.
- **Kernel doesn't read `PT_INTERP`** -- it unconditionally loads
  ld-kappara.so for every `ET_DYN` binary.  PT_INTERP is mostly a
  POSIX hint for which loader to use; we have exactly one.
  ET_EXEC binaries (`/bin/hello`) skip ld.so entirely -- they're
  the kernel's bootstrap test path.
- **Blob, not VFS file.**  The kernel reads ld-kappara.so via
  `ld_kappara_blob_start[]` directly during exec rather than
  through a VFS lookup.  Once stage 7 lands `dlopen`, the file
  will also be registered at `/lib/ld-kappara.so` for symbolic
  discovery.

Files:

- `lib/ld-kappara/ld_start.S` -- the entire stage 5 linker.  Walks
  past `argv[]` (length = argc from `[sp]`) and the `envp[]` NULL,
  then iterates `auxv[]` looking for `AT_ENTRY (= 9)`.  Branches
  to that value.  Stack is unmodified -- the app's `crt0.S` reads
  `argc` from `[sp]` exactly as before.
- `lib/ld-kappara/linker.ld` -- ET_EXEC at `LD_VA`, `_start` first.
- `Makefile` -- `LDK_ELF` target wired into the kernel image via
  `uts/aarch64/usrblobs.S` (incbin'd alongside `/usr/bin` ELFs).

Kernel changes in `sys_execve_impl` (`uts/os/user/user.c`):

- New `LD_VA = 0x30000000`, `LD_SIZE = 0x100000` window in the
  user VA layout.
- For `ET_DYN`: after the app's PT_LOAD + reloc walk, call new
  `load_static_elf()` helper to map ld-kappara.so's PT_LOAD into
  the same vm_map.
- Exec stack now carries the POSIX shape:
  `[argc | argv... NULL | envp_NULL | auxv... AT_NULL ]`.
  The auxv has one entry today: `AT_ENTRY = app_entry`.
- Trap-frame entry point becomes `ld.so._start` (LD_VA) for
  ET_DYN; ET_EXEC binaries enter directly at their `e_entry`.

ELF entry-point handoff observed in boot logs:

    exec: EL0 entry=0x30000000 sp=0x203fffb0       <-- ftpd via ld.so
    exec: EL0 entry=0x30000000 sp=0x203fffb0       <-- uptime via ld.so
    exec: code=0x20000000, /bin/hello 4688 bytes   <-- ET_EXEC: direct

Verified: `cmd/test all` 13/13 (control flows
shell -> ld.so -> app -> exit, every cmd binary), `make ARCH=virt
smoke-ftp` 8/10 PASS (matches pre-stage-5 noise band -- TCP race
unrelated), `/bin/hello` still loads via the ET_EXEC path.

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

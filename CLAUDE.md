# Notes for Claude when working in this repo

## House rules

- This is a **SVR4-flavored Unix-like kernel**, not Linux-flavored.
  When given a choice between the two design lineages, prefer SVR4 /
  Solaris idioms (vnodes, cdevsw[major], STREAMS modules with
  qinit/streamtab, M_HANGUP / M_PROTO / M_DATA message types, vnode
  v_count + vop_inactive lifecycle, DEC/BSD reliable signals with
  sigaction).  Diverge only when SVR4's shape is genuinely more
  complex AND the gain doesn't justify it; if you do, note it
  explicitly in the commit body.

- **Always update the docs when changing code.**  Specifically:

  | When you change…                       | Update…                                  |
  |----------------------------------------|------------------------------------------|
  | Shell command set in `user/init.c`     | `docs/SHELL.md`                          |
  | `ked` behavior                         | `docs/KED.md`                            |
  | `/proc/*` or `/dev/*` entries          | `docs/PROCFS.md`                         |
  | Kernel internals (sched, vfs, streams, signals, mmu, boot) | `docs/ARCHITECTURE.md` |
  | Build/run/QEMU flags                   | `docs/BUILDING.md`                       |
  | Top-level layout, big-picture status   | `README.md`                              |

  This is non-optional: a commit that changes a public-facing
  command, a syscall number, a major number, a /proc file, or a
  build target without updating the relevant docs is incomplete.
  Treat the doc edit as part of the same commit, not a follow-up.

## Build invariants

- `make ARCH=aarch64` produces `build/aarch64/kernel8.img`.
- Two-pass link populates `.kallsyms` for backtraces.  Don't reorder
  the linker script around `.kallsyms` — its position **after** BSS
  is what makes the two-pass scheme work.
- `-MMD -MP` + `-include $(DEPS)` is in the Makefile.  Adding a new
  field to a struct in a shared header should automatically rebuild
  every dependent .o.  If it doesn't, the `-include` got moved
  before `.DEFAULT_GOAL := all` again — fix that, don't paper over.

## Testing convention

- Default test rig:
  ```
  /tmp/test_in.sh | timeout NN qemu-system-aarch64 -M raspi3b \
      -serial mon:stdio -serial null -display none \
      -kernel build/aarch64/kernel8.img 2>&1 | tail -N
  ```
- For input-driven tests, the script does `sleep N; echo "command"`
  pairs — fbcon is disabled by default so the boot is fast enough
  that 3-4 second sleeps are plenty before the first command.
- Boot to prompt: ~3 seconds in QEMU TCG.

## Hot bug-class reminders

- **Slab returns recycled memory, not zero pages.**  If you add fields
  to a struct that gets `kmalloc`'d (kthread, file, inode, ...), make
  sure the allocator zeros via `kmemset` or each caller sets every
  field.  See commit `983e1c2` for the canonical bug.
- **`struct file_ops` growth is an ABI break** between .o files unless
  `make clean` runs.  With `-MMD -MP` this is now automatic — don't
  remove it.
- **`context_switch` saves/restores DAIF per thread** (`switch.S`).
  Don't remove the daif slot from the saved-register frame; the
  sleep/wake path depends on each thread carrying its own IRQ-mask
  state.  See commit `0929814`.
- **Secondary cores in `.Lpark` use `wfi`, not `wfe`** (boot.S) —
  WFE wakes on barrier instructions anywhere in the system; in QEMU
  TCG that means tight-spinning a host CPU per parked vCPU.  Don't
  re-add `msr daifset` before the WFI — that segfaults at least one
  QEMU version when secondary cores are still at EL2.  See
  `aa8759f`.
- **EL0 faults must NOT panic the kernel.**  `trap_dispatch` checks
  `vec_id == VEC_SYNC_LO64` and calls `sys_exit_impl` after marking
  SIGSEGV.  EL1 faults still panic — those are kernel bugs.
- **The trap-exit epilogue must run with IRQs masked**
  (`uts/aarch64/vectors.S`, `KERNEL_EXIT` macro; same shape in
  `aarch64_enter_userspace`).  The sequence is `msr elr_el1, …` /
  `msr spsr_el1, …` / … / `eret`.  If an IRQ fires between the
  ELR write and the ERET, AArch64 hardware silently overwrites
  ELR_EL1 with the resume-here kernel PC; when the nested IRQ
  eret's back, the outer epilogue's final ERET reads that stale
  ELR_EL1 and jumps EL0 to a kernel address — instruction abort
  at `trap_tail+0xc`.  Don't remove the `msr daifset, #2` at the
  top of `KERNEL_EXIT` or `aarch64_enter_userspace`.  See commit
  for the canonical fix.

## Commit message style

- Imperative subject, 60 chars max.
- Body explains **why**, not just what.  Reference prior commits
  by their short hash when relevant (the project leans on that
  history a lot).
- Don't put model-id / co-author tags in commit messages.

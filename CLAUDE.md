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

- `make` produces `build/kernel.img`.  Single ARCH (QEMU `virt`,
  which is also the AWS Graviton on-ramp -- see `docs/AWS.md`).
  Pi-specific drivers moved to `attic/raspi3b/` and the
  `ARCH=aarch64` build path is retired.
- Two-pass link populates `.kallsyms` for backtraces.  Don't reorder
  the linker script around `.kallsyms` — its position **after** BSS
  is what makes the two-pass scheme work.
- `-MMD -MP` + `-include $(DEPS)` is in the Makefile.  Adding a new
  field to a struct in a shared header should automatically rebuild
  every dependent .o.  If it doesn't, the `-include` got moved
  before `.DEFAULT_GOAL := all` again — fix that, don't paper over.

## Testing convention

- Canonical "is HEAD healthy?": `make test`.  Runs `smoke-ftp +
  smoke-sdk + smoke-linux + smoke-linux-mmap` plus `cmd/test all
  14/14` and reports a single "ALL TESTS PASS" line.
- Run interactively: `make run` (Ctrl-A x to quit QEMU).
- Drive specific shell commands:
  ```
  /tmp/test_in.sh | timeout NN qemu-system-aarch64 \
      -M virt,gic-version=3 -cpu cortex-a72 -nographic -m 256 \
      -netdev user,id=n0 -device virtio-net-device,netdev=n0 \
      -kernel build/kernel.img 2>&1 | tail -N
  ```
- For input-driven tests, the script does `sleep N; echo "command"`
  pairs — boot is fast enough that 3-4 second sleeps are plenty
  before the first command.
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
- **Refcounts that are touched from multiple CPUs are lock-free
  atomics, not plain `int`.**  Every counter where multiple threads
  can race the increment/decrement — `struct file::f_refs`,
  `struct inode::i_count` today — goes through
  `atomic_inc(&p)` / `atomic_dec_and_test(&p)` in
  `include/kappara/atomic.h`.  Never write raw `++` / `--`; one
  lost decrement either leaks the object or double-frees it,
  and the symptom (close hook running on already-freed memory)
  is delayed enough that it presents as random later corruption.
  The canonical wrappers are `file_get` / `file_put` for files
  and `vfs_iget` / `vfs_iput` for inodes — those are the only
  acceptable touchpoints.  If you add a new shared refcount,
  follow the same shape and document it here.
- **SMP-shared allocator/freelist state needs a spinlock.**  pmm
  and kmem are guarded by `pmm_lock` and `kmem_lock`.  Lock
  order is **kmem → pmm** (`grow_cache` holds `kmem_lock` across
  `pmm_alloc`).  If you add any other multi-word state that all
  CPUs touch (a global queue, a hash table, etc.), pick a lock
  for it before the first SMP test, not after.

## Commit message style

- Imperative subject, 60 chars max.
- Body explains **why**, not just what.  Reference prior commits
  by their short hash when relevant (the project leans on that
  history a lot).
- Don't put model-id / co-author tags in commit messages.

---
name: doc-sync
description: Audit current changes against kappara's mandatory doc-update table from CLAUDE.md. Use after kernel edits, before commits, or when the user asks "what docs need updating?", "doc audit", "did I miss any docs?", "doc sync". Surfaces concrete missing doc edits — not vague reminders.
---

# Doc-sync audit

CLAUDE.md is explicit: "a commit that changes a public-facing command, a syscall number, a major number, a /proc file, or a build target without updating the relevant docs is **incomplete**. Treat the doc edit as part of the same commit, not a follow-up."

## Mapping table (authoritative)

| Touched files / change                                              | Required doc                |
|---------------------------------------------------------------------|-----------------------------|
| `user/init.c` — new/changed shell command, help text                | `docs/SHELL.md`             |
| `user/ked.c`, `user/ked_*.c` — editor behavior                      | `docs/KED.md`               |
| `kernel/proc.c`, new `/proc/*` STREAMS cdev                         | `docs/PROCFS.md`            |
| New `/dev/*` major (touches `cdevsw.h`)                             | `docs/PROCFS.md`            |
| `kernel/sched.c`, `kernel/vfs.c`, `kernel/stream*.c`, `kernel/sig*.c`, `kernel/pmm.c`, `kernel/vmm.c`, `arch/*/mmu.c`, `arch/*/boot.S`, anything in `kernel/smp/` | `docs/ARCHITECTURE.md` |
| `Makefile` (build targets, CFLAGS, ARCH), QEMU rig changes          | `docs/BUILDING.md`          |
| Top-level structure, status, what-works list                         | `README.md`                 |
| ftrace API or workflow                                              | `docs/FTRACE.md`            |
| New syscall number (`include/kappara/syscalls.h`)                   | `docs/ARCHITECTURE.md` (Syscall section) |

## Procedure

1. Collect touched files:
   ```sh
   git diff --name-only HEAD
   git diff --cached --name-only
   ```
2. For each touched code file, look up required docs from the table.
3. Cross-reference: are those docs also in the diff?
4. **Report shape:**
   - **OK**: list code-file → doc pairs that are covered
   - **MISSING**: code-file → doc that needs an edit, with a one-sentence suggestion of what to add (e.g. "user/init.c added `cmd_foo` → docs/SHELL.md needs an entry under 'Commands' between `ftrace` and `help`")
   - **NOT NEEDED**: changes that don't trigger any rule (e.g. internal kernel refactor with no public surface)

## Output discipline

- Be concrete. "Update SHELL.md" is useless. "SHELL.md needs an entry for `cmd_ftrace` with synopsis + the four sub-verbs" is useful.
- Don't lecture about the policy — the user knows. Just give the audit result.
- If the doc edit is already in the diff but the content looks thin, say so once and stop.

---
name: svr4-check
description: Review current kernel changes against SVR4/Solaris idioms and the known hot bug classes from CLAUDE.md. Use when the user asks to "review", "lint", "check my changes", "any issues?", or before commits on kernel code. Catches Linux-style idioms creeping in, missing slab zeroing, EL1-vs-EL0 panic mistakes, and the well-known regression traps.
---

# SVR4 idiom + hot-bug review

This skill audits the working-tree diff. It does **not** rubber-stamp — if nothing is wrong, say so explicitly. It does **not** invent issues to look thorough.

## House lineage (CLAUDE.md)

> SVR4-flavored Unix kernel, **not** Linux-flavored. Prefer Solaris idioms.
> Diverge only when SVR4's shape is genuinely more complex AND the gain
> doesn't justify it; if you do, note it explicitly in the commit body.

## Idioms to prefer

- `vnode` + `v_op`, not VFS dispatch table-of-tables
- `cdevsw[major]`, `struct file_ops` (our name), STREAMS `qinit`/`streamtab`
- `M_HANGUP` / `M_PROTO` / `M_DATA` / `M_IOCTL` mblk types
- `v_count` refcount + `vop_inactive` on last close
- BSD/DEC reliable signals via `sigaction` (no `signal()` semantics)
- Per-CPU = `struct cpu` (Solaris `cpu_t` shape) reached via `curcpu()` → `TPIDR_EL1`
- Service queues for deferred work, not tasklets / softirqs

## Linux idioms to flag

| Linux pattern         | Our equivalent                          |
|-----------------------|-----------------------------------------|
| `struct file_operations` | `struct file_ops`                    |
| tasklet / softirq / BH | STREAMS service queue (`qenable`)      |
| `current` macro       | `curthread` (via `curcpu()->cpu_thread`)|
| `__this_cpu_*`        | direct field on `curcpu()`             |
| `kmalloc(... GFP_*)`  | plain `kmalloc(sz)` + remember to zero |
| `printk(KERN_*)`      | `kprintf("...")`                       |

## Hot bug classes — check the diff for each

1. **Slab returns recycled memory, not zero pages.**
   New field on a `kmalloc`'d struct (`kthread`, `file`, `inode`, `vnode`, `cpu`, ...)? Confirm either:
   - allocator does `kmemset(p, 0, sz)`, OR
   - **every** caller sets the new field.
   Canonical regression: commit `983e1c2`.

2. **`struct file_ops` growth without rebuild** breaks ABI between `.o` files. `-MMD -MP` catches it now, but if `kappara/file.h` (or whichever header defines it) changed and any `.o` is still old, expect crashes. Flag if you see header changes without a hint that callers were recompiled.

3. **`context_switch` saves/restores DAIF per thread** (`arch/aarch64/switch.S`). The DAIF slot in the saved-register frame is load-bearing — sleep/wake depends on each thread carrying its own IRQ-mask state. Don't remove it. See `0929814`.

4. **Secondary cores at `.Lpark` use `wfi`, not `wfe`** (`arch/aarch64/boot.S`). WFE wakes on barrier instructions anywhere in the system → in QEMU TCG that means one host CPU pinned per parked vCPU. Also: don't re-add `msr daifset` before the WFI — segfaults at least one QEMU version when secondaries are still at EL2. See `aa8759f`.

5. **EL0 faults must NOT panic the kernel.**
   `trap_dispatch` must check `vec_id == VEC_SYNC_LO64` and route to `sys_exit_impl` with SIGSEGV. EL1 faults still panic — those are kernel bugs. If the diff touches trap routing, verify the EL0 branch is intact.

6. **`uart_lock` is shared by kprintf and the console driver.**
   New output path? Use `uart_acquire` + `uart_putc_unlocked` + `uart_release` (batch a logical unit) or one of the locked wrappers. Don't introduce a per-character `uart_putc()` loop in a hot path.

7. **Panic path bypasses locks.**
   Anything called from `kpanic` must use `uart_puts_panic` and not touch `uart_lock`. Another CPU may have crashed holding it.

## Procedure

1. `git diff HEAD` (and staged). If empty, say so and stop.
2. Walk the patch hunk-by-hunk.
3. For each hunk, classify against the idiom list and bug-class list.
4. Output as a flat list:
   ```
   file.c:LN — <bug-class N or idiom rule> — <one-line concrete fix>
   ```
5. End with a one-line verdict: `OK`, `OK with comments`, or `BLOCKING: <count> issues`.

## Output discipline

- If a hunk is fine, don't mention it.
- Don't restate the rule when reporting — the rule list above is canonical, point to it by number.
- Don't review formatting / style / naming unless something is genuinely confusing.
- Don't ask the user to "consider" things — either it's a defect or it isn't.

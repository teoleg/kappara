---
name: build
description: Build the kappara kernel. Use whenever the user asks to "build", "compile", "make", "make sure it compiles", or after source edits to confirm the tree still builds. Defaults to ARCH=aarch64. Accepts args `arm`, `clean`, `trace` (enables -finstrument-functions).
---

# Build kappara

## Inputs

Args (any combination):
- `aarch64` (default) | `arm`
- `clean` → `make clean` first
- `trace` → `make TRACE=1` (enables ftrace -finstrument-functions)
- `v` → don't suppress make output

## Procedure

1. If `clean` requested: `make clean ARCH=<arch>`.
2. Build:
   ```sh
   make ARCH=<arch> [TRACE=1] -j$(nproc) 2>&1 | tee /tmp/kappara-build.log
   ```
3. Inspect the log:
   - Success → report `built ARCH=<arch>: build/<arch>/kernel8.img` + size in KB.
   - First error → report `file:line — message`. If a header struct grew, suggest `make clean` (see invariants).
   - Warnings → count. If `clean` was used, list new ones; otherwise stay quiet about pre-existing noise.

## Invariants (do not paper over)

- **Two-pass link populates `.kallsyms`.** Don't reorder the linker script around `.kallsyms` — its position **after BSS** is what makes the two-pass scheme work.
- **`-MMD -MP` + `-include $(DEPS)`** must remain in Makefile. Adding a field to a shared struct should auto-rebuild every dependent `.o`. If it doesn't, `-include` got moved before `.DEFAULT_GOAL := all` again — fix that, don't paper over.
- **`struct file_ops` growth is an ABI break** between `.o` files unless `make clean` runs. The `-MMD -MP` dep tracking now catches this — don't remove it.
- **ARM build is currently link-broken** (`sys_spawn_impl`/`sys_exit_impl` are aarch64-only). Expected; report and stop.

## Quick failure triage

- `undefined reference to <sym>` → check Makefile object list for missing `.o`
- `error: invalid use of incomplete type` → likely forward declaration mismatch in a header
- `relocation truncated to fit` → linker section ordering changed; bisect linker.ld
- Massive cascading errors → likely a header parse error early; surface only the first

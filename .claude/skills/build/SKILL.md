---
name: build
description: Build the kappara kernel. Use whenever the user asks to "build", "compile", "make", "make sure it compiles", or after source edits to confirm the tree still builds. Single-arch (QEMU virt → AWS Graviton via docs/AWS.md). Accepts args `clean`, `trace`.
---

# Build kappara

Kappara is single-arch as of the raspi3b retirement: one image,
QEMU `virt` for development, eventually boots on AWS Graviton via
the AWS.md staged transition.  Output is `build/kernel.img`.

## Inputs

Args (any combination):
- `clean` → `make clean` first
- `trace` → `make TRACE=1` (enables ftrace -finstrument-functions)
- `v` → don't suppress make output

## Procedure

1. If `clean` requested: `make clean`.
2. Build:
   ```sh
   make [TRACE=1] -j$(nproc) 2>&1 | tee /tmp/kappara-build.log
   ```
3. Inspect the log:
   - Success → report `built: build/kernel.img` + size in KB.
   - First error → report `file:line — message`. If a header struct
     grew, suggest `make clean` (see invariants).
   - Warnings → count. If `clean` was used, list new ones; otherwise
     stay quiet about pre-existing noise.

## Invariants (do not paper over)

- **Two-pass link populates `.kallsyms`.** Don't reorder the linker
  script around `.kallsyms` — its position **after BSS** is what
  makes the two-pass scheme work.
- **`-MMD -MP` + `-include $(DEPS)`** must remain in Makefile.
  Adding a field to a shared struct should auto-rebuild every
  dependent `.o`. If it doesn't, `-include` got moved before
  `.DEFAULT_GOAL := all` again — fix that, don't paper over.
- **`struct file_ops` growth is an ABI break** between `.o` files
  unless `make clean` runs. The `-MMD -MP` dep tracking now catches
  this — don't remove it.

## Quick failure triage

- `undefined reference to <sym>` → check Makefile object list for
  missing `.o`
- `error: invalid use of incomplete type` → likely forward
  declaration mismatch in a header
- `relocation truncated to fit` → linker section ordering changed;
  bisect linker.ld
- Massive cascading errors → likely a header parse error early;
  surface only the first

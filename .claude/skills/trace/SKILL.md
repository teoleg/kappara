---
name: trace
description: Capture an ftrace recording from a kappara boot or a specific scenario. Use when the user asks to "trace", "ftrace", "capture a trace", "what functions ran during X?", or wants to debug a specific code path. Drives the in-kernel ftrace via `/proc/ftrace` and the `ftrace` shell verb.
---

# Capture an ftrace recording

The kernel has a per-CPU ring buffer in BSS (256 events × 4 CPUs), works before MMU/pmm are up. Controlled via `/proc/ftrace` (STREAMS cdev) and the `ftrace on|off|reset|dump` shell command.

## Inputs

Args: optional scenario command(s) to run between `ftrace on` and `ftrace dump`.
- No args → assume the user wants the boot trace (ftrace is auto-enabled at the start of `kmain` via `ftrace_init`, so `ftrace dump` immediately captures it).
- With args → enable, run scenario, dump.

## Procedure

1. **Verify TRACE build.** ftrace needs `-finstrument-functions`. Check the latest build log or just rebuild:
   ```sh
   make ARCH=aarch64 TRACE=1 -j$(nproc)
   ```
   If the existing `build/aarch64/kernel8.img` was built without TRACE=1, the rings will stay empty — rebuild.

2. **Compose the input script.** For a boot-trace (no args):
   ```sh
   sleep 3
   echo "ftrace dump"
   sleep 2
   ```
   For a scenario trace:
   ```sh
   sleep 3
   echo "ftrace reset"
   sleep 1
   echo "ftrace on"
   sleep 1
   echo "<scenario>"
   sleep 1
   echo "ftrace off"
   sleep 1
   echo "ftrace dump"
   sleep 2
   ```

3. **Run via the `test` skill rig.** Capture stdout, then post-process: extract the `ftrace dump` section (between the dump start marker and shell prompt).

4. **Report:**
   - Total events captured (per CPU)
   - First 10 events (with symbol names from kallsyms)
   - Last 10 events
   - Any suspicious patterns (tight `__cyg_profile_func_enter` loops, deep nesting without exit pairs)
   - If counts are 0 on all CPUs: tell the user TRACE=1 was probably missing.

## How it works (background — only if user asks)

- Ring storage: `static struct ftrace_event rings[4][256]` in BSS — zeroed by boot.S, no allocator dependency
- Event: `{ ts (cntpct_el0), fn, caller, cpu_kind }`
- CPU id: `mrs mpidr_el1` & 0xff — no scheduler / TPIDR_EL1 needed
- Re-entry guard: `in_dump[cpu]` flips while formatting the dump (otherwise the formatter itself would record events into the ring it's reading)
- Opt-out: files in `NOINST_OBJS` in the Makefile (ftrace.c, printk.c, string.c, kallsyms.c, uart.c) get `-fno-instrument-functions` to avoid recursion/noise
- Rings wrap silently — only the last 256 events per CPU survive

## Limits / pitfalls to surface

- 256 events/CPU is small. If the scenario is long, only the **tail** survives.
- Timestamps are raw cycles. Convert with the timer frequency if asked.
- IRQ handlers are NOT excluded — long traces may be dominated by timer-tick instrumentation.
- The shell `ftrace dump` reads `/proc/ftrace` once; for fresh data, the user must `reset` first.

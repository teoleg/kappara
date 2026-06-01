---
name: test
description: Run the kappara QEMU boot+shell test rig and report what happened. Use whenever the user wants to verify a kernel change boots, asks to "test it", "try it", "boot", "smoke test", "run it", or "does it still work?". Optionally pass shell commands as args (newline-separated, or `;`-separated) to drive the user-space init shell after boot.
---

# Run kappara on QEMU raspi3b

Standard test rig from CLAUDE.md. Boot to prompt is ~3s in QEMU TCG.

## Inputs

Args are zero or more shell commands to feed `init` after boot.
- No args → just boot and capture splash + prompt.
- `;`-separated or newline-separated → one `echo` + 1s sleep per command.

## Procedure

1. **Verify build artifact exists.** If `build/aarch64/kernel8.img` is absent, run `make ARCH=aarch64 -j$(nproc)` first. If that fails, stop and report the build error — don't proceed to QEMU.

2. **Generate `/tmp/test_in.sh`:**
   ```sh
   #!/bin/sh
   sleep 3                  # boot
   echo "<cmd1>"
   sleep 1
   echo "<cmd2>"
   ...
   sleep 1                  # drain
   ```
   `chmod +x /tmp/test_in.sh`. If no args, generate a script with only `sleep 5`.

3. **Compute timeout.** Base 5s, +1s per command, +1s drain. Cap at 30s unless the user asked for longer.

4. **Run:**
   ```sh
   /tmp/test_in.sh | timeout <T> qemu-system-aarch64 -M raspi3b \
       -serial mon:stdio -serial null -display none \
       -kernel build/aarch64/kernel8.img 2>&1 | tail -200
   ```

5. **Report.** Keep it tight:
   - One line: "Booted to prompt OK" / "PANIC at <symbol>" / "Hang — no prompt seen"
   - For each command issued, one line with its output (or "no output")
   - Surface anomalies: kernel faults, unexpected RIP, character interleaving, dead CPUs
   - If clean and uninteresting, just say "clean boot, prompt OK" — don't paste the splash

## Notes

- `timeout` exits 124 on hit. That's the expected exit code — not a failure signal.
- `fbcon` is disabled by default (boot is fast). The kernel prints to UART only.
- Useful diagnostic targets:
  - `cat /proc/ps` — thread list, see all idle CPUs
  - `cat /proc/meminfo` — slab + pmm health
  - `cat /proc/ftrace` — captured trace (needs TRACE=1 build)
- The shell prompt is `kappara:/# `.

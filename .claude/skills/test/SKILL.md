---
name: test
description: Run the kappara QEMU boot+shell test rig and report what happened. Use whenever the user wants to verify a kernel change boots, asks to "test it", "try it", "boot", "smoke test", "run it", or "does it still work?". Optionally pass shell commands as args (newline-separated, or `;`-separated) to drive the user-space init shell after boot.
---

# Run kappara on QEMU virt

Single-arch test rig.  `make` produces `build/kernel.img`; that
image boots in QEMU `virt` today and is the basis for the AWS
Graviton port (see docs/AWS.md).  Boot to prompt is ~3s in QEMU TCG.

For the canonical "is HEAD healthy?" check, prefer `make test` --
it runs all four smoke targets plus `cmd/test all` 15/15 and
reports a single "ALL TESTS PASS" line.  Use this skill when you
want to drive specific commands into the shell or inspect a
particular boot transcript.

## Inputs

Args are zero or more shell commands to feed `init` after boot.
- No args → just boot and capture splash + prompt.
- `;`-separated or newline-separated → one `echo` + 1s sleep per command.

## Procedure

1. **Verify build artifact exists.** If `build/kernel.img` is absent,
   run `make -j$(nproc)` first.  If that fails, stop and report the
   build error -- don't proceed to QEMU.

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
   `chmod +x /tmp/test_in.sh`. If no args, generate a script with
   only `sleep 5`.

3. **Compute timeout.** Base 5s, +1s per command, +1s drain. Cap
   at 30s unless the user asked for longer.

4. **Run:**
   ```sh
   /tmp/test_in.sh | timeout <T> qemu-system-aarch64 \
       -M virt,gic-version=3 -cpu cortex-a72 -nographic -m 256 \
       -global virtio-mmio.force-legacy=false \
       -netdev user,id=n0 \
       -device virtio-net-device,netdev=n0 \
       -kernel build/kernel.img 2>&1 | tail -200
   ```

   Or simpler, if you only need stdio interaction (no FTP/telnet):
   ```sh
   /tmp/test_in.sh | timeout <T> make run 2>&1 | tail -200
   ```

5. **Report.** Keep it tight:
   - One line: "Booted to prompt OK" / "PANIC at <symbol>" / "Hang -- no prompt seen"
   - For each command issued, one line with its output (or "no output")
   - Surface anomalies: kernel faults, unexpected RIP, character interleaving, dead CPUs
   - If clean and uninteresting, just say "clean boot, prompt OK" -- don't paste the splash

## Notes

- `timeout` exits 124 on hit. That's the expected exit code -- not a failure signal.
- The kernel prints to UART (`-nographic` serial) only.
- Useful diagnostic targets:
  - `cat /proc/ps` -- thread list, see all idle CPUs
  - `cat /proc/meminfo` -- slab + pmm health
  - `cat /proc/ftrace` -- captured trace (needs TRACE=1 build)
- The shell prompt is `kappara:/# `.
- For a full pre-commit health check: `make test`.

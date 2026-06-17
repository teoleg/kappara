---
name: test-telnet
description: Run the kappara virt-arch QEMU + nc-over-telnet rig and report what happened. Use whenever the user wants to verify a change still lets external telnet reach the shell on tty4, or to drive shell commands through the in-kernel telnetd path. Boots `build/virt/kernel.img` headless in the background, waits for the "telnetd: listening" banner, then drives `nc localhost 2323` with the supplied commands.
---

# Run kappara virt + nc-over-telnet

Companion to the raspi3b `/test` skill but exercises the virtio-net +
TCP + telnetd path end-to-end.  The kernel image is `ARCH=virt` and
the shell lives on `/dev/tty4` reached via host port 2323
(`hostfwd=tcp::2323-:23`).

## Inputs

Args are zero or more shell commands sent over the telnet session.
- No args → just boot, connect, capture the banner + prompt, disconnect.
- `;`-separated or newline-separated → one `printf "<cmd>\r"` + 1s
  pause per command.

## Procedure

1. **Verify build artifact exists.**  If `build/virt/kernel.img` is
   absent, run `make ARCH=virt -j$(nproc)` first.  If that fails,
   stop and report the build error — don't proceed to QEMU.

2. **Verify `nc` is on PATH.**  If not, suggest
   `apt install netcat-openbsd` and stop.

3. **Compute timeouts.**
   - QEMU lifetime cap: 12s + 2s per command, max 60s.
   - nc client cap: 6s + 2s per command, max 30s.

4. **Boot QEMU virt headless in the background:**
   ```sh
   timeout <TQ> qemu-system-aarch64 \
       -M virt,gic-version=3 -cpu cortex-a72 -nographic -m 256 \
       -global virtio-mmio.force-legacy=false \
       -netdev user,id=n0,hostfwd=tcp::2323-:23 \
       -device virtio-net-device,netdev=n0 \
       -kernel build/virt/kernel.img > /tmp/qemu_telnet.log 2>&1 &
   QPID=$!
   ```
   Use a unique-enough log path so concurrent runs don't trample
   each other.

5. **Wait for `telnetd: listening`** to appear in the log (poll once
   per second, up to 10s).  If it never appears, kill QEMU and
   report `tail -30 /tmp/qemu_telnet.log`.

6. **Drive nc:**  feed a small script of `printf "<cmd>\r"; sleep 1`
   pairs into `timeout <TN> nc localhost 2323 > /tmp/nc_out.txt`.
   The trailing `\r` is what ldterm wants for end-of-line.

7. **Tear down:** `kill $QPID 2>/dev/null; wait 2>/dev/null`.

8. **Report.**  Keep it tight:
   - One line: "shell reached over telnet OK" / "telnetd never came
     up" / "nc could not connect (kernel logs:)" / "session ended
     with no prompt".
   - The `/tmp/nc_out.txt` contents (paste only the interesting
     lines — strip the banner if it's just the standard banner).
   - Surface anomalies from the QEMU log: kernel faults, hung
     thread switches, ARP unresolved, TCP RST.

## Notes

- `timeout` exits 124 on hit; that's the expected end of run, not
  a failure signal.
- The kernel splash + boot trace go to `/tmp/qemu_telnet.log`,
  NOT stdout, because nc is using stdio.  Always include relevant
  tail of the log in the report when something goes wrong.
- Single client at a time — the kernel telnetd is single-shot
  between accepts.  Don't run two `test-telnet` invocations in
  parallel against the same image.
- The kernel side sees telnet input as keystrokes; commands need
  `\r` not `\n`.  ldterm converts `\r` → `\n` via ICRNL on the
  way to the shell.
- If you only want to inspect kernel boot/init traces and don't
  care about telnet, use `/test` (raspi3b) instead — that path
  is faster.

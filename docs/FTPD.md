# FTPD plan -- foreign binaries onto a running kappara

This is the working plan for how externally-built binaries get from
the developer's host into a running kappara instance.  The end goal:

    host$ aarch64-linux-gnu-gcc -o foo foo.c
    host$ ftp 127.0.0.1 2121
    ftp> put foo
    kappara:/# /home/foo
    hello from foo!

It is **not** a finished design doc; it is a roadmap that gets
checked off as each step lands.  When a step is complete, its
status line flips from `[ ]` to `[x]` and a one-line summary plus
the commit hash goes under it.  When a step's design changes mid-
flight, the change goes in the doc, not buried in commits.

## Architecture choices (locked)

These were decided up front to avoid backtracking:

- **FTP, RFC 959 control protocol.**  Not TFTP, not raw netcat.
  Real FTP exists in real SVR4 (`in.ftpd`); it gives us a session
  protocol, separate control + data channels, and a recognisable
  client (`ftp(1)`, `lftp`, FileZilla, curl `ftp://`).  More code
  than a netcat shortcut, but the code is the point.

- **Passive mode only.**  PASV opens a fresh listener on a dynamic
  port; the client connects to it.  No PORT / active mode.  This
  side-steps having the server connect outbound to the client and
  matches what every modern firewall expects anyway.

- **User-space daemon, not kernel kthread.**  Telnetd lives in
  `uts/virt/telnetd.c` because it bridges TCP to an existing
  tty hook -- it has to be in-kernel to reach the tty driver
  pointers.  ftpd has no such tie: it's pure
  open/putmsg/getmsg/read/write/close.  Running it in EL0:
   1. Forces our TPI surface to actually be usable by EL0 code
      (it's currently exercised only by in-kernel callers like
      telnetd and the TCP selftest).
   2. Matches the SVR4 shape -- inetd-style daemons run as
      user processes, not as kernel threads.
   3. If ftpd crashes it doesn't take the kernel with it.

- **Uploads land in `/home`, a separate kfs ramdisk.**  We don't
  want STOR to be able to overwrite a /usr/bin binary out from
  under a running process, or to let an unauthenticated FTP
  client smash `/etc/motd`.  `/home` is a fresh kfs region with
  its own backing block device; mkdir/creat/write all already
  work in kfs, we just need a second mount.

  After upload, the user can `exec /home/foo` or copy/move into
  /usr/bin manually if they want it on the standard path.

- **No auth, USER/PASS accepted as a no-op.**  This is a lab
  setup behind QEMU's NAT.  USER/PASS are part of the protocol
  grammar so we accept and respond, but we don't validate.
  Authentication can come later behind a single `etc/passwd`
  lookup once we have a user/group concept at all.

## What needs to be in place before ftpd can work

These are prerequisites, in dependency order:

### Step 1 -- EL0 TPI dial-out: `cmd/tcpconnect.c`

Status: `[x]` -- shipped (see commit log).

Built `cmd/tcpconnect.c`: opens `/dev/tcp`, puts T_TCP_BIND_REQ
(port 0 for ephemeral), gets T_TCP_BIND_ACK, puts T_TCP_CONN_REQ
to the target, gets T_TCP_CONN_CON, then a half-duplex line loop
(read stdin / send T_TCP_DATA_REQ / drain one T_TCP_DATA_IND /
print).  `.` on a line tears the session down via ORDREL.

End-to-end verified on virt: `tcpconnect 10.0.2.2 12345` reaches
a host `nc -l -p 12345`, host PONG appears in shell, typed lines
land on host.

Bugs flushed by writing this:
- `stream_getmsg` was non-blocking -- it returned -1 immediately
  if no message was queued, so an EL0 caller doing "putmsg
  request; getmsg response" lost the race when the module's
  reply hadn't reached `sd_rq` yet.  Now wraps getq in a
  `sd_readwait`-locked sleep loop the same shape as
  `stream_read`.
- Don't `I_PUSH "tcp"` after `open("/dev/tcp")`: stream_head's
  driver-open path autopushes it.  A redundant push stacks two
  tcp modules and IP_T_BIND_REQ loops through `tcp_wq_putp`
  twice instead of reaching `ip_wput`.  Code now opens and
  proceeds straight to T_TCP_BIND_REQ; comment in the source
  flags the autopush.

Carry-overs for the bidirectional `nc`-equivalent (out of scope
for step 1, useful later for ftpd's data channel):
- Need select / poll or a worker thread to mix stdin and TCP
  reads.  Either ship `sys_spawn` through libc, or extend
  `getmsg` with a non-blocking flag.

### Step 2 -- Second writable kfs region mounted at `/home`

Status: `[x]` -- shipped.

What landed:
- Added a second 512 KB ramdisk (`ramdisk1`) with its own
  storage, name, and `bd_read`/`bd_write` shims.
- Refactored kfs from a single static `kfs_mnt` to a small
  per-`bd` slot table (`MAX_KFS_MOUNTS = 4`).  The bitmap
  cache is now keyed by `bd` pointer so `/usr/bin` and `/home`
  no longer stomp each other's free-block view.  Every
  `kfs_bit_get/set/save` call takes a `struct kfs_mnt *`;
  `kfs_alloc_block_zero` / `kfs_free_blocks` look up the
  right mnt from the `bd` they were handed.
- `exec_space_init` now mkimages ramdisk1 empty and mounts
  it at `/home`.
- `main.c` ramdisk_home_init runs alongside ramdisk_init,
  before exec_space_init (else mkimage zeroes get clobbered).

Verified end-to-end:
- raspi3b: `touch /home/foo; echo /home/foo hi; cat /home/foo`
  roundtrips a written file.  `cmd/test all` still 13/13.
- virt via telnet: same flow over the tcp-bridged shell on
  tty4.  `ls /` shows `home/` alongside `usr/`, `bin/`, etc.

Carry-overs for step 3:
- `kfs_dir_creat` allocates a fixed `KFS_BLOCKS_PER_FILE = 64`
  slot per file (32 KB cap).  Plenty for `cmd/*` ELFs (current
  largest is ~50 KB and that's `cmd/test.c` with every selftest
  inside) but may bite if anyone STORs a multi-MB blob.  Track
  this when ftpd starts seeing real uploads.
- `regfile_write` appends sequentially; there's no truncate-
  on-open in kfs yet.  STOR over an existing file would tail
  with stale bytes from the previous version.  Either drop in
  `unlink` first (cheap) or wire `O_TRUNC` through to a real
  `kfs_truncate` (cleaner, do in step 3 when ftpd needs it).

### Step 3 -- `cmd/ftpd.c`: the daemon itself

Status: `[x]` -- end-to-end shipped (small files clean; throughput
cliff above ~5 KB tracked as a separate follow-up below).

What landed:
- `cmd/ftpd.c`: long-running EL0 daemon driving an RFC 959
  control state machine.  Commands handled: USER, PASS, SYST,
  FEAT, TYPE, PWD/XPWD, CWD, PASV, LIST, NLST, RETR, STOR, NOOP,
  QUIT.  Everything else returns 502.
- PASV-only; PORT (active) is intentionally NAK'd.  PASV picks
  one of eight ports in 30000..30007 and listens via the same
  TPI dance telnetd uses in-kernel, driven from EL0 via
  putmsg / getmsg.
- STOR refuses any target not under `/home/` -- belt-and-suspenders
  on top of the ramdisk1 separation.  `unlink` first to dodge
  the missing-O_TRUNC carry-over from step 2.
- `user/init.c`: tty0's init calls `sys_execve("/usr/bin/ftpd")`
  before entering the shell loop, so ftpd comes up as a sibling
  process and the shell keeps working.  Only fires for `my_tty==0`
  so we don't spawn five copies.
- `Makefile`: ARCH=virt QEMU_ARGS grew `hostfwd=tcp::2121-:21`
  plus `hostfwd=tcp::3000N-:3000N` for N=0..7.

Bugs flushed by writing this:
- `stream_getmsg` was truncating data when the segment exceeded
  the caller's `data->maxlen` and silently freeing the rest --
  fine for tiny TPI ACK ctl mblks, ruinous for STOR of a
  >MSS-sized chunk.  Now advances `dptr->b_rptr` by the bytes
  delivered and putbq's the leftover with a freshly-cloned
  M_PROTO ctl head, so the next getmsg sees the same
  T_TCP_DATA_IND it would have seen on the original delivery.
- Pipelined CRLF lines arrived in a single TCP segment but
  `recv_line` only returned the first line and lost the rest in
  a stack-local buffer.  Now uses a static pending-bytes stash
  reset per session.  Single-client server, so a single static
  is enough -- when we go multi-client this has to live on
  `struct session`.
- QEMU user-mode NAT means the host reaches us at 127.0.0.1
  through `hostfwd=...`, not 10.0.2.15.  Advertising 10.0.2.15
  in the PASV reply made `ftp(1)` reject with "passive mode
  address mismatch".  Now lies with `127,0,0,1` so the host's
  hostfwd table routes the data port back through NAT.
  Switch the `ADV_IP_*` defines for a real network deployment.

Verified on virt:
- Banner -> USER -> PASS -> SYST -> TYPE I -> CWD /home -> PASV
  -> STOR small.txt (27 B, 226 transfer complete) -> QUIT.
- Side-channel verify via telnet (`nc localhost 2323`):
  `ls /home` shows the uploaded file, `cat /home/small.txt`
  reads the bytes back identical to host source.

Carry-overs:
- Throughput cliff fixed.  Files up to 32 KB now upload at
  4-8 MiB/s and download byte-identical at 2-3 MiB/s.  Real
  ELFs uploaded over FTP run cleanly: `exec /home/foo` after
  STOR'ing `build/cmd/ifconfig.elf` prints the eth0 / lo0 dump
  as expected.  Four kernel bugs flushed by chasing the cliff:
  - TCP had no "user drained" notification, so once the receive
    queue filled to TCP_RCV_WND_MAX the window stayed closed
    forever.  Added `struct stdata::sd_on_drain(sd, arg)`
    invoked by `stream_read` / `stream_getmsg` after consuming
    bytes; `tcp_qopen` wires it to `tcp_on_user_drain` which
    fires a pure ACK so the peer learns the window re-opened.
    `tcp_qclose` clears the hook before kfree-ing the TCB.
  - `TCP_RCV_WND_MAX` (8 KB) and `TCP_SND_BUF_MAX` (8 KB) were
    both too small for an FTP transfer.  Bumped to 65535 each
    to match the uint16 wire-window cap when wscale=0.
  - `data_accept` in ftpd was closing the PASV listener fd
    right after `do_accept`, which dropped the IP-level bind
    that the responder child shares with the listener for
    fan-out.  Subsequent ACKs hit a port that's no longer bound
    and IP dropped them, stalling the transfer at exactly the
    first ~4 segments.  Split into `data_accept` + `data_done`
    so the listener stays alive until after the data fd is
    `do_close`'d.
  - New `sd_on_drain`/`sd_on_drain_arg` fields in struct stdata
    weren't initialised in `stream_build` / `pipe_end`; slab
    returned recycled memory and `stream_read` then BLR'd
    through a garbage function pointer (canonical CLAUDE.md
    "slab returns recycled memory, not zero pages" trap).
    Explicit NULL-init in both build paths.
- `do_close` on the data channel during STOR does the polite
  ORDREL roundtrip; we could shortcut on the data fd since
  the file is already on disk, but leave it for now -- it's
  not the slowness root cause.
- Multi-client: only one session at a time, by design.

### Step 4 -- Host-side recipe + smoke test

Status: `[x]` -- shipped.

What landed:
- `make ARCH=virt run-ftp`: boots virt headless in the background,
  waits for `ftpd: listening`, then drops you into `ftp -p 127.0.0.1
  2121` with USER/PASS/cd/put hints printed.  Ctrl-C tears the
  background QEMU down via the trap on EXIT.
- `make ARCH=virt smoke-ftp`: scripted gate.  Boots virt, STORs a
  small text file, RETRs it back, byte-compares, then STORs
  `build/cmd/ifconfig.elf` and verifies `exec /home/foo` over
  telnet prints the expected `eth0` line.  Exits non-zero on any
  step's failure so CI can flag regressions.
- Makefile `QEMU_ARGS` already had the FTP-related hostfwds (control
  21 -> host 2121, PASV pool 30000..30007 -> same on host).

Operating recipe written under "Operating recipe" below.

Carry-over closed:
- The "RETR returns 512-byte zero block injected mid-stream"
  bug was racy IRQ vs `handle_data_req` snd_buf reassignment.
  Reading `old = b_wptr - b_rptr` and then `kmemcpy(.., b_rptr, old)`
  is not atomic against a tcp_rput ACK trim that advances
  `b_rptr` (so the kmemcpy reads `old` bytes from the new,
  smaller position -- spilling past `b_wptr` into the tail of
  the allocb, often zero).  Fixed by fencing the whole snd_buf
  swap with DAIF mask/restore.  smoke-ftp now byte-compares
  the full `build/cmd/ifconfig.elf` (11776 B) and passes
  reproducibly.

### Step 5 -- Optional follow-ups (out of scope for v1)

Status: `[ ]`

Once steps 1-4 are green, the natural extensions:
- Real auth (USER lookup against an etc/passwd, even fake one).
- `chmod`/permission concept so STOR'd files start non-executable.
- Persist `/home` across reboot (needs real storage -- SD/EMMC
  driver on raspi3b, virtio-blk on virt).
- `ls` over LIST -- proper "drwxr-xr-x  ..."  output instead of
  bare names.
- TLS (FTPS) -- well, no.  This is a learning kernel.

## Operating recipe

Host packages needed once:

    apt install qemu-system-arm gcc-aarch64-linux-gnu netcat-openbsd ftp

End-to-end push-and-run:

    # Build everything (kernel + userland + ftpd ELF embedded in /usr/bin)
    make ARCH=virt -j$(nproc)

    # Terminal A: boot kappara + drop into ftp(1)
    make ARCH=virt run-ftp
    ftp> user anonymous any
    ftp> binary
    ftp> cd /home
    ftp> put my_program             # lands at /home/my_program
    ftp> bye

    # Terminal B: telnet to kappara's shell and exec what you just put
    nc localhost 2323
    kappara:/# exec /home/my_program

Scripted gate (CI / pre-commit):

    make ARCH=virt smoke-ftp        # exits 0 on PASS, non-zero on any step's failure

If `run-ftp` says `ftpd did not come up`, tail
`/tmp/kappara-virt.log` -- that's where the headless QEMU's
serial output goes.  The most common cause is a stale QEMU
holding the FTP hostfwd ports from a previous run; `make stop`
or `pkill -x qemu-system-aarch64` clears it.

Host-side firewall: QEMU `hostfwd` binds to **0.0.0.0** by
default, so other machines on your network can reach the FTP
service if you punch holes for ports 2121 + 30000..30007.  Edit
`QEMU_ARGS` in the Makefile to `hostfwd=tcp:127.0.0.1:...` if you
want loopback-only.

## Open questions to revisit later

- Should ftpd be supervised?  If it crashes, init has no
  respawn loop today.  This is the first non-shell EL0 service;
  the answer probably becomes "we need a tiny supervisor /
  init.d".
- The kfs ramdisk for /home shares pmm with everything else.
  Bounding STOR size matters once we don't trust the client.

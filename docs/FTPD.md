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

Status: `[ ]`

Why first: ftpd's data channel needs both PASV (listen + accept,
already exercised by telnetd) AND the active-open path (because
RETR sends bytes the client requested -- we don't dial out for
that, the client does for PASV -- but EVERY TPI roundtrip from
EL0 needs to work).  No EL0 code today does the full open + I_PUSH
"tcp" + bind + connect + read/write + close cycle.  We need to
prove it works before building anything on top of it.

What to build:
- `cmd/tcpconnect.c`: standalone ELF, behaves like `nc <ip> <port>`.
- Open `/dev/tcp`, `ioctl(I_PUSH, "tcp")`, putmsg `T_TCP_BIND_REQ`
  with port 0 (ephemeral), getmsg expect `T_TCP_BIND_ACK`.
- putmsg `T_TCP_CONN_REQ` with target IP+port, getmsg expect
  `T_TCP_CONN_CON`.
- Then a select-style loop: poll stdin (fd 0) and the TCP fd;
  shovel one direction to the other.  No real select() yet --
  alternating non-blocking reads with sys_yield is enough.

Test:
- On host: `nc -l 12345` (or python `socket.recv` loop).
- In kappara: `tcpconnect 10.0.2.2 12345`, type a line, see it
  on the host.  Type a line on the host, see it in kappara.

Gaps this likely surfaces:
- EL0 calling `putmsg`/`getmsg` with TPI primitives in the ctl
  buffer + raw bytes in the data buffer -- we need both to
  flow through `sys_putmsg_impl` / `sys_getmsg_impl` correctly.
- Whether the existing user/init.c read_one / cwrite path can
  share fds with the TCP stream cleanly.
- Anything ldterm-shaped that assumes the fd is a tty.

### Step 2 -- Second writable kfs region mounted at `/home`

Status: `[ ]`

What to build:
- A second `struct block_device` -- another ramdisk or a new
  bram block area.
- `kfs_mkimage` it empty (no payloads); `kfs_mount` it at
  `vfs_mkdir(root, "home")`.
- Verify from shell: `cd /home`, `echo foo bar`, `cat /home/foo`.
- Verify the kfs WRITE path goes through (existing
  `regfile_write` + `kfs_dir_creat`).

Why now: STOR has to land somewhere, and we DON'T want it
landing on the existing `/usr/bin` ramdisk where binaries get
exec'd.  This also flushes any latent bugs in kfs write that
the shell `echo > file` path doesn't reach (large writes, many
small writes, file-already-exists overwrite).

Gaps this likely surfaces:
- kfs may not have a "truncate on open" path -- STOR over an
  existing file would leave stale tail bytes.  Add an O_TRUNC
  flag or have STOR `unlink` first.
- The `bram` block device's size cap may bite once we start
  uploading ELFs (each `cmd/*.c` ELF is ~10-40 KB).

### Step 3 -- `cmd/ftpd.c`: the daemon itself

Status: `[ ]`

What to build:
- Long-running EL0 process, listens on TCP port 21.
- On each accept, fork (or spawn) a worker that drives the
  RFC 959 control-channel state machine on that connection.
- Commands handled (everything else gets `502 Command not
  implemented`):
  - `USER any` -> `331 Password required`
  - `PASS any` -> `230 Logged in`
  - `SYST` -> `215 UNIX Type: L8`
  - `TYPE I` -> `200 Binary mode`
  - `PWD` -> `257 "/" is the current directory`
  - `CWD /home` -> chdir, `250 OK`
  - `LIST` -> open data channel, send `ls -l` style listing
  - `PASV` -> listen on a dynamic port from the PASV pool,
    reply `227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)`
  - `RETR name` -> open data channel, send file bytes
  - `STOR name` -> open data channel, write bytes to /home/name
  - `QUIT` -> `221 Bye`, close
- Spawn from `user/init.c` at the same point we spawn telnetd's
  prereqs (post-network-up).
- Make stdio go to a dedicated /dev/ttyN or to /dev/klog so
  log lines from ftpd don't fight the active console.

Gaps this likely surfaces:
- We don't have `fork()` from EL0 -- only `sys_spawn` (single
  thread, shared everything in the calling AS).  Per-connection
  worker has to be a thread, not a process.  That's fine for a
  lab tool but means SIGINT propagation between sessions is
  shared.
- PASV needs a port range.  Pick 30000..30009 and add ten
  `hostfwd=tcp::30000-:30000` style entries to the Makefile,
  or just one for now and accept one transfer at a time.
- We never built a "select / poll" interface; the worker
  has to alternate read tries.  That's tolerable here because
  FTP control is line-at-a-time and data channel is one-way
  per transfer.

### Step 4 -- Host-side recipe + smoke test

Status: `[ ]`

What to build:
- Update Makefile QEMU_ARGS for ARCH=virt: add `hostfwd=tcp::2121-:21`
  and the PASV-port hostfwd(s).
- `make ARCH=virt run-ftp` target that boots virt and prints a
  one-line "connect with `ftp 127.0.0.1 2121`" hint.
- A scripted smoke test (`/tmp/ftp_test.sh`) that pushes a known
  binary via `ftp` and then verifies `kappara:/# /home/foo`
  produces expected output.
- Document the recipe in this file (below the plan section).

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

## Operating recipe (filled in as steps complete)

(Will be populated as step 4 lands.)

## Open questions to revisit later

- Should ftpd be supervised?  If it crashes, init has no
  respawn loop today.  This is the first non-shell EL0 service;
  the answer probably becomes "we need a tiny supervisor /
  init.d".
- The kfs ramdisk for /home shares pmm with everything else.
  Bounding STOR size matters once we don't trust the client.

# ksh — kappara shell reference

`ksh` is the userspace shell, running at EL0.  The kernel spawns one
instance per `/dev/ttyN` (4 by default), so there's a separate shell
PID waiting on each virtual console: `user-init-0` on tty0,
`user-init-1` on tty1, etc.  Each shell opens its own `/dev/ttyN` as
fds 0/1/2.  `uart_rx_main` routes UART bytes only to the active tty,
so the three background shells sit BLOCKED in `sys_read` until you
`Ctrl-X N` to switch the active console.

Each shell prints a prompt with its current working directory:
`kappara:/#`.  The opening banner names the tty, e.g.
`kappara shell on tty0 -- type 'help' for commands`.

Line editing supports backspace and **up/down arrow** for command
history (last 16 lines).  Globals like `cwd`, `ked`/`vi` state, etc.
are shared across all four shells; since only the active tty drives
input, this matters in practice only if you `Ctrl-X` mid-edit.

## File system

| Command                    | What it does                                                |
|----------------------------|-------------------------------------------------------------|
| `ls [path]`                | List directory entries (names only).                         |
| `ll [path]`               | "ls -l": each row is `type size name`.  For chrdevs the size column shows `major,minor`. |
| `cd [path]`                | Change directory.  `.`, `..`, mixed `..` are all canonicalized.  No arg = `/`.  The target is probed with `sys_ls` first -- non-existent paths and regular files are rejected without mutating cwd. |
| `pwd`                      | Print working directory.                                    |
| `cat <path>`               | Dump a file's bytes to the console.                         |
| `echo <path> <text>`       | Overwrite a file with `<text>` (creates if missing).        |
| `vc <n> <text>`            | Write `<text>` to `/dev/tty<n>` (one-digit minor) without doing the cwd-relative open the `echo` command does.  Lets you put bytes into an inactive virtual console's cell buffer; switch to that tty via `Ctrl-X <n>` to see them.  See ARCHITECTURE.md "Virtual consoles" for the bigger picture. |
| `append <path> <text>`     | Append `<text>` + newline to a file.                        |
| `touch <path>`             | Create an empty file under a kfs mount.                     |
| `mkdir <path>`             | Create a directory under a kfs mount.                       |
| `rm <path>`                | Remove a regular file (kfs blocks reclaimed via the bitmap).|
| `rmdir <path>`             | Remove an empty directory.                                  |
| `ked <path>`               | Tiny ed-like line editor.  See [KED.md](KED.md).             |
| `vi <path>`                | Modal full-screen editor (vi-lite).  See [VI.md](VI.md).     |
| `kc`                       | Two-panel file manager, Norton-Commander-style.  Blue background, F-keys along the bottom.  See the `kc` section below. |

## Processes

| Command                    | What it does                                                |
|----------------------------|-------------------------------------------------------------|
| `pid`                      | Print the current thread id.                                |
| `spawn [arg]`              | Spawn a long-running worker thread in init space (returns its tid). |
| `kill <tid> [sig]`         | Send a signal (POSIX numbers).  Default is `SIGTERM=15`.    |
| `exec <path> [args...]`    | Load an ELF64 executable into the exec address space (VA 0x20000000), spawn an exec thread, and `sys_wait` for it.  The exec'd program inherits the shell's fd table so stdout (fd 1) goes to `/dev/console`.  Trailing words are passed through as `argv` to the child's `main(argc, argv)`; `argv[0]` is the resolved path. |
| `halt`                     | Ask QEMU to exit (semihosting SYS_EXIT). Run targets in the Makefile pass `-semihosting-config enable=on,target=native`. |
| `ftrace [on\|off\|reset\|dump]` | Per-CPU function tracer.  No arg = `dump` (alias for `cat /proc/ftrace`).  Only meaningful when the kernel was built with `make TRACE=1`.  See `docs/FTRACE.md`. |
| *(unknown)*                | If a command is not a builtin, the shell tries `exec /usr/bin/<name>` automatically with the original `argv` (so the program's `argv[0]` is the bare command name, not the full path).  Type any `/usr/bin/` binary name without the `exec` prefix. |

## /usr/bin — standalone ELF programs

These programs are ELF blobs embedded in the kernel image (in
`uts/aarch64/usrblobs.S`) that get copied at boot into a kfs ramdisk
mounted at `/usr/bin`.  Typing their name without `exec` works because
the shell's PATH fallback automatically tries `/usr/bin/<name>` for any
unrecognised command.  Source lives in `cmd/`.

| Program             | What it does                                            |
|---------------------|---------------------------------------------------------|
| `ps`                | Read and display `/proc/ps` (thread list with TID/state/name). |
| `ping [ip] [count]` | Send ICMP echo requests via `/dev/icmp`.  Default target `127.0.0.1`, count 4. |
| `ifconfig`          | Dump `/proc/netif` (one row per registered netif).      |
| `netstat`           | Dump `/proc/netif` + `/proc/slip` + `/proc/tcp`.        |
| `test <name>`       | Run one selftest (see subcommand list below).  `test all` runs every test, `test` with no args prints the list. |

The five `/usr/bin` programs cap at `KFS_DIRENTS=14`; adding a new
top-level ELF burns a kernel-image slab plus a kfs slot, so new
selftests go into `cmd/test.c` as another entry in the registry,
not a new ELF.

`test` subcommands (the registry lives in `cmd/test.c`):

| Subcommand     | What it does                                                |
|----------------|-------------------------------------------------------------|
| `fork`         | parent forks; child mutates a global, exits with status 42; parent waits and verifies the mutation didn't leak across the address-space boundary. |
| `wait`         | spawn a short-lived worker thread via `sys_spawn`, `sys_wait` for it. |
| `sig`          | install a SIGTERM handler, signal self, verify delivery (smoke test for `sigaction`/`sendsig`/`sigreturn`). |
| `mask`         | `sigprocmask` + `sigsuspend` round-trip: block SIGTERM, queue it, atomically unblock-and-wait. |
| `segv`         | spawn a worker that installs a SIGSEGV handler then deref NULL -- proves one-shot handler delivery. |
| `pipe`         | create a pipe, write a message, read it back in the same thread. |
| `pipework`     | spawn writer + reader threads connected by a pipe; demonstrates blocking-read EOF detection. |
| `malloc`       | exercise libc `malloc`/`free`/`calloc`/`realloc` end-to-end. |
| `udp`          | TPI smoke test for `/dev/udp`: bind, send, recv, verify payload round-trip. |
| `pktfilt`      | composability proof: open `/dev/udp`, `I_PUSH` the pktfilter module at runtime, configure it to drop one dst port, verify the drop counter, `I_POP` it, verify traffic flows again. |
| `tcp`          | TPI smoke test for `/dev/tcp`: 3-fd flow (listener / client / responder), 3-way handshake, bidirectional data, graceful close. |
| `tcpmulti`     | multi-accept proof: two clients connect to one listener, listener stays in LISTEN, both accepted onto separate responder fds, cross-traffic verified. |

The `crash` shell builtin is unchanged: `crash` from the shell prompt
derefs NULL directly and the EL0 fault path SIGSEGVs the calling
thread.  See `docs/ARCHITECTURE.md` for the trap dispatch.

Exec-space programs can call `sys_spawn` to create sub-threads.  The
kernel allocates their stacks from `exec_stack_storage` starting at
`EXEC_STACK_TOP − slot × 64 KB` (same layout as the init-space spawn
pool).  `exec_spawn_next` resets to 0 at the start of each `sys_execve`
call so every exec'd program gets a fresh pool.

`/usr/bin` programs are linked against the freestanding `lib/libc` (see
`docs/ARCHITECTURE.md`).  `crt0.S` parses the exec stack and calls
`int main(int argc, char **argv)`; `argv[0]` is the resolved path (or
the command name for PATH-fallback dispatch).  The libc provides
`printf`/`puts`, `FILE*` (`fopen`/`fread`/`fwrite`/`fprintf`,
`stdin`/`stdout`/`stderr` on fds 0/1/2), and `malloc`/`free` against
a free-list allocator backed by `SYS_brk`.

## Streams / device I/O

These all act on a single "current" fd kept in the shell, set by `open`.

| Command                    | What it does                                                |
|----------------------------|-------------------------------------------------------------|
| `open <path>`              | `sys_open(path, 0)`; remembers the fd for the next commands.|
| `close`                    | Close the remembered fd.                                    |
| `read [n]`                 | Read up to `n` bytes from the fd (default 64).              |
| `write <text...>`          | Write text to the fd (no trailing newline).                 |
| `push <module>`            | `ioctl(I_PUSH, "module")` — push a module on the stream.    |
| `pop`                      | `ioctl(I_POP)`.                                             |

## Examples

A small session that exercises most of it:

```
kappara:/# ls
usr
etc
bin
proc
dev
kappara:/# ll /usr/bin
reg   17920 test
reg    9456 netstat
reg    9712 ifconfig
reg   10144 ping
reg    9456 ps
kappara:/# test malloc
malloc: PASS
kappara:/# test tcp
tcp: PASS
kappara:/# netstat
NAME    FLAGS  MTU     IP              NETMASK
lo0     UP      1500  127.0.0.1       255.0.0.0
slip0   UP       296  192.168.10.2    255.255.255.252
iface:  slip0  (mini-UART at 0x3F215040)
rx_bytes_total:         0
...
STATE         LPORT  PEER             EXTRAS
```

## `kc` — Norton Commander-style file manager

Two-panel file manager.  Lives in `user/kc.c`, textually `#include`d
from `user/init.c` so it shares the shell's helpers (`cwrite`,
`cputc`, `cwd`, `resolve_path`, `path_canon`, ...) without a separate
user binary.

### Layout

Hardcoded 80x24, double-line borders (UTF-8 box drawing) with the
title embedded in the top border, Norton-style:

```
╔═════════════ /home/user ═════════════╦═══════════ Info ═══════════════╗
║ ..                            UP-DIR ║   Kappara Commander -- Info    ║
║ etc                              DIR ║                                ║
║ motd                              31 ║   Name: motd                   ║
║ readme                            93 ║   Path: /home/user/motd        ║
║                                      ║                                ║
║                                      ║   Type: regular file           ║
║                                      ║   Size: 31 bytes               ║
║                                      ║                                ║
║                                      ║   -- content ----              ║
║                                      ║   welcome to kappara! ...      ║
║                                      ║                                ║
║ ... (rows 2..21)                     ║   ... (info content)           ║
╚══════════════════════════════════════╩════════════════════════════════╝
 /home/user/motd  31 bytes
 1Help 2Menu 3View 4Edit 5Copy 6RenMv 7Mkdir 8Delet 9PullDn qQuit
```

Left panel is the file listing (navigable with arrow keys); right
panel is the **Info pane** showing the properties of the highlighted
entry: name, full path, and either a directory summary or, for
regular files, a content preview.  The preview is sniffed first:

- **Text** files (no NUL, no 0x7F, no control bytes outside
  HT/LF/CR in the first 256 bytes) get rendered as plain text with
  non-printables stripped, up to ~512 bytes.
- **Binary** files get a compact hex dump:
  ```
    -- hex ----
    000  7f 45 4c 46 02 01 01 00 .ELF....
    008  03 00 b7 00 01 00 00 00 ........
    ...
  ```
  8 bytes per row + ASCII column on the right, ~14 rows shown
  (112 bytes total).  Non-printable ASCII bytes are rendered as `.`.

Classic Norton Commander palette: panel background **blue** (ANSI
`\033[44m`), regular files in light grey, directories in bold
bright-white, selected entry inverted to black-on-cyan,
function-key footer black-on-cyan.

### Keys

| Key                | Action                                        |
|--------------------|-----------------------------------------------|
| Arrow up / down    | Move cursor within active panel.              |
| Arrow left / right | Same as up / down (vim-style not yet wired).  |
| PgUp / PgDn        | Page through entries.                         |
| Home / End         | Jump to first / last entry.                   |
| `Tab`              | Switch active panel.                          |
| `Enter`            | Enter directory; on a regular file, view it.  |
| `F1` or `1`        | Help overlay.                                 |
| `F3` or `3`        | View file (pager: `q`/Esc to dismiss).        |
| `F4` or `4`        | Edit selected file in `vi` (synchronous call; kc re-renders on `:wq`). |
| `F5` or `5`        | Copy selected file to the other panel's dir.  |
| `F6` or `6`        | Move / rename (copy + unlink, no rename syscall yet). |
| `F7` or `7`        | Make directory (prompts for name).            |
| `F8` or `8`        | Delete (file → `unlink`, dir → `rmdir`).      |
| `F9`, `F2`         | Pull-down menu / panel menu -- stubs.         |
| `q`, `Q`, `Esc`, `Ctrl-X`, `F10` | Quit and return to the shell.  F10 is the classic NC binding but terminals and QEMU's GTK display routinely steal it -- prefer `q` or `Ctrl-X`. |

F-key escape sequences accepted: VT100 `ESC O P/Q/R/S` (F1..F4) and
xterm `ESC [ 11~ ... 21~` (F1..F10).  Number keys 1..0 fire F1..F10
unconditionally as a fallback for terminals whose function keys send
something exotic.

### Implementation notes

- Listing is parsed from `sys_ll`'s output.  The size field is
  8-char right-justified (or `MMMM,NNN` for chrdevs), so the parser
  splits each line at the *last* space before `\n` — everything to
  the right is the name, everything to the left is metadata.
- All paint paths funnel through an 8 KB output buffer (`kc_obuf`)
  flushed in 1 KB chunks (the kernel's `kmalloc` caps at the
  size-2048 slab; bigger writes get rejected with
  `kmalloc(N): too large`).  Without the buffer, each `cputc`
  was its own `sys_write` and a full repaint took multiple seconds.
- `kc` is invoked as a function call, not an exec — there is no
  fork/exec yet.  When it returns, control goes back to the shell's
  dispatch loop.

## Ctrl-C handling

The shell installs a SIGINT handler at startup.  Pressing Ctrl-C
(byte 0x03) gets intercepted in `uart_rx_main` before the byte
reaches the stream -- the kernel SIGINTs whichever thread is the
foreground reader of `/dev/console` (the one blocked on the read
wait queue, or the last reader if nobody is blocked right now).
The shell's handler prints `^C\r\n` and sets a flag; `read_line`
checks the flag on the next iteration, clears its partial input,
and re-prompts.

Caveats: vi / ked / kc share the SIGINT handler since they live in
the same address space.  Don't press Ctrl-C inside an editor; use
its own quit command.  See `docs/ARCHITECTURE.md` for the wider
signal model.

## How input is implemented

- Every keystroke arrives in the PL011 RX FIFO.
- `uart_rx_main` (a kernel thread) polls the FIFO, allocates an `mblk_t`,
  and `putnext`s it into `/dev/console`'s STREAMS stack.
- The shell is blocked in `sys_read(fd_console)` on the stream head's
  read wait queue.  The arrival wakes it; the byte gets echoed by the
  shell as it appends to the line buffer.

The whole input path is SVR4 STREAMS — `uart_rx_main` is just a tiny
driver-shaped feeder, and you could `I_PUSH` a line discipline (e.g.
the existing `upper` module) onto `/dev/console` to mangle every
keystroke on its way to the shell.

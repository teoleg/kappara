# ksh — kappara shell reference

`ksh` is the userspace shell, running at EL0 as PID 2.  It opens
`/dev/console` for input/output, prints a prompt, reads a line via the
blocking STREAMS path, tokenizes on spaces, and dispatches to one of
the commands below.  Line editing supports backspace and **up/down
arrow** for command history (last 16 lines).

The prompt shows the current working directory: `kappara:/etc#`.

## File system

| Command                    | What it does                                                |
|----------------------------|-------------------------------------------------------------|
| `ls [path]`                | List directory entries (names only).                         |
| `lsl [path]`               | "ls -l": each row is `type size name`.  For chrdevs the size column shows `major,minor`. |
| `cd [path]`                | Change directory.  `.`, `..`, mixed `..` are all canonicalized.  No arg = `/`.  The target is probed with `sys_ls` first -- non-existent paths and regular files are rejected without mutating cwd. |
| `pwd`                      | Print working directory.                                    |
| `cat <path>`               | Dump a file's bytes to the console.                         |
| `echo <path> <text>`       | Overwrite a file with `<text>` (creates if missing).        |
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
| `spawn [arg]`              | Spawn a long-running worker thread (returns its tid).       |
| `kill <tid> [sig]`         | Send a signal (POSIX numbers).  Default is `SIGTERM=15`.    |
| `crash`                    | Spawn a thread that dereferences NULL (tests SIGSEGV path). |
| `sigtest`                  | Install a SIGTERM handler, signal self, prove handler ran and execution resumed (smoke test for `sigaction`/`sendsig`/`sigreturn`). |
| `masktest`                 | Round-trip `sigprocmask` + `sigsuspend`: block SIGTERM, send to self, then unblock-and-wait atomically; handler runs inside `sigsuspend` and the original mask comes back on the way out. |
| `waittest`                 | Spawn a short-lived worker and call `sys_wait` for it; demonstrates the join shape. |
| `segvtest`                 | Install a SIGSEGV handler in a spawned worker, deref NULL, prove the handler runs once (`SA_RESETHAND` is auto-applied for EL0 fault delivery). |
| `exec <path>`              | Load an ELF64 executable (e.g. `/bin/hello`) into the exec address space (VA 0x20000000), spawn an exec thread, and `sys_wait` for it to exit.  The exec'd program inherits the shell's fd table so stdout (fd 1) goes to `/dev/console`. |
| `halt`                     | Ask QEMU to exit (semihosting SYS_EXIT). Run targets in the Makefile pass `-semihosting-config enable=on,target=native`. |
| `ftrace [on\|off\|reset\|dump]` | Per-CPU function tracer.  No arg = `dump` (alias for `cat /proc/ftrace`).  Only meaningful when the kernel was built with `make TRACE=1`.  See `docs/FTRACE.md`. |

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

## Pipes / IPC

| Command                    | What it does                                                |
|----------------------------|-------------------------------------------------------------|
| `pipe`                     | sys_pipe demo: write then read in the same shell thread.    |
| `pipework`                 | `sys_pipe` + two spawned workers connected through it (blocking read + EOF). |

## Examples

A small session that exercises most of it:

```
kappara:/# ls
etc
proc
dev
kappara:/# cd /etc
kappara:/etc# lsl
reg       31 motd
reg       93 readme
reg       50 hello.txt
kappara:/etc# cat motd
no soup for you, only streams.
kappara:/etc# echo greeting hello world
wrote 12 bytes
kappara:/etc# cat greeting
hello world
kappara:/etc# ked greeting
ked: /etc/greeting (1 lines)
* p
1: hello world
* a
this is the second line
.
* w
35 bytes
* q
kappara:/etc# cd ..
kappara:/# cat /proc/ps
  TID  STATE   NAME
    0  READY   main
    1  READY   uart_rx
    2  RUN     user-init
```

## `kc` — Norton Commander-style file manager

Two-panel file manager.  Lives in `user/kc.c`, textually `#include`d
from `user/init.c` so it shares the shell's helpers (`cwrite`,
`cputc`, `cwd`, `resolve_path`, `path_canon`, ...) without a separate
user binary.

### Layout

Hardcoded 80x24:

```
+--------- Row 1: header bar -------------+-----------------------+
|/path/of/left          /path/of/right                            |
+ Rows 2..21: 20 entries per panel ------+-----------------------+
| ..                            DIR     | etc                DIR  |
| motd                           31     | proc               DIR  |
| readme                         93     | dev                DIR  |
+----- Row 22: separator -----------------------------------------+
| Row 23: status -- /selected/path  31 bytes                      |
| Row 24: 1Help 2Menu 3View 4Edit 5Copy 6RenMv 7Mkdir 8Delet ...   |
+-----------------------------------------------------------------+
```

Classic Norton Commander palette: panel background is **blue** (ANSI
`\033[44m`), regular files in light grey, directories in bold
bright-white, selected entry inverted to black-on-cyan, active-panel
header bold black-on-white, function-key footer black-on-cyan.

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

- Listing is parsed from `sys_lsl`'s output.  The size field is
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

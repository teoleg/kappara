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
| `cd [path]`                | Change directory.  `.`, `..`, mixed `..` are all canonicalized.  No arg = `/`. |
| `pwd`                      | Print working directory.                                    |
| `cat <path>`               | Dump a file's bytes to the console.                         |
| `echo <path> <text>`       | Overwrite a file with `<text>` (creates if missing).        |
| `append <path> <text>`     | Append `<text>` + newline to a file.                        |
| `touch <path>`             | Create an empty file under a kfs mount.                     |
| `mkdir <path>`             | Create a directory under a kfs mount.                       |
| `rm <path>`                | Remove a regular file (kfs blocks reclaimed via the bitmap).|
| `rmdir <path>`             | Remove an empty directory.                                  |
| `ked <path>`               | Tiny ed-like line editor.  See [KED.md](KED.md).             |

## Processes

| Command                    | What it does                                                |
|----------------------------|-------------------------------------------------------------|
| `pid`                      | Print the current thread id.                                |
| `spawn [arg]`              | Spawn a long-running worker thread (returns its tid).       |
| `kill <tid> [sig]`         | Send a signal (POSIX numbers).  Default is `SIGTERM=15`.    |
| `crash`                    | Spawn a thread that dereferences NULL (tests SIGSEGV path). |

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

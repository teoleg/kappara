# ked — kappara editor

`ked` is a tiny line editor in the spirit of Unix `ed(1)`.  It loads a
file into a fixed in-memory line buffer, drops into an ed-style
sub-prompt, and lets you print, append, insert, delete, and save.

## Invocation

```
kappara:/etc# ked motd
ked: /etc/motd (1 lines)
* _
```

The `*` is ked's prompt.  Press Enter to advance one line, or type a
command.

Limits: **64 lines x 120 chars** per buffer.  Files larger than that
get truncated on load.  Long-term, this grows; for now it's enough for
short config files.

## Commands

### Addresses

Most commands accept an address or range before the command letter:

| Form        | Meaning                                          |
|-------------|--------------------------------------------------|
| `N`         | Line number N (1-based)                          |
| `.`         | Current line                                     |
| `$`         | Last line in the buffer                          |
| `N,M`       | Range from N to M inclusive                      |
| `,`         | All lines (`1,$`)                                |

### Print

```
*  p          print current line
*  3p         print line 3
*  1,5p       print lines 1 through 5
*  ,p         print everything
```

A bare number or `.`/`$` (no command) advances the current line and
prints it — so just hitting Enter walks through the file.

### Append / insert

```
*  a
your text...
more text...
.
```

`a` enters insert mode after the current line; everything you type goes
into the buffer until a line containing **just a period** ends it.
`i` works the same but inserts *before* the current line.  `Na` inserts
after line N (`0a` inserts at the very top).

### Delete

```
*  d          delete current line
*  3d         delete line 3
*  2,4d       delete lines 2 through 4
```

### Save / quit

```
*  w          write buffer back to the file (prints "NN bytes")
*  q          quit (refuses if there are unsaved changes)
*  q!         force quit, discard changes
```

## Example session

```
kappara:/# cd /etc
kappara:/etc# touch poem
kfs_creat: 'poem' in dir_block=2 slot=3 blocks=15..18
kappara:/etc# ked poem
ked: /etc/poem (0 lines)
* a
the quick brown fox
jumps over the lazy dog
and lives happily ever after
.
* ,p
1: the quick brown fox
2: jumps over the lazy dog
3: and lives happily ever after
* 2d
* ,p
1: the quick brown fox
2: and lives happily ever after
* w
49 bytes
* q
kappara:/etc# cat poem
the quick brown fox
and lives happily ever after
```

## Why ed and not vi?

Ed's command grammar is tiny — a single character per command, plus
an optional address.  No screen-update logic, no terminfo, no escape
sequences.  Insert mode reads lines until a lone `.`, which is the
classic Unix idiom and the only sentinel that's safe in a kernel where
the only display is the same byte stream as input.  When kappara
eventually has terminal modes (raw / cooked / line discipline), `vi`
becomes plausible.  Until then `ed` is the right shape.

## Implementation notes

- The buffer is a fixed 64x120 static array in `user/init.c`; no
  allocator in userspace yet.
- The command-line and insert-mode line readers are a stripped-down
  `simple_read_line()` that doesn't support history or arrow keys —
  ed-mode shouldn't store stray VT100 escape bytes in your file.
- `w` opens the file with `O_TRUNC`, rewrites every line, writes a
  trailing `\n`, and closes.  Allocation is the existing kfs
  fixed-size pre-allocation (`KFS_BLOCKS_PER_FILE` blocks per file).

# vi — kappara modal editor (lite)

A stripped-down vi-like editor for kappara.  Same line-buffer
backing store as `ked` but with full-screen VT100 rendering and
modal editing.

```
kappara:/etc# vi motd
```

## Modes

| Mode      | Status bar shows | What you can do                                |
|-----------|------------------|------------------------------------------------|
| **NORMAL** (default) | `-- NORMAL --` | Move, delete, switch to insert / command |
| **INSERT**           | `-- INSERT --` | Type characters in; backspace eats one  |
| **COMMAND**          | `-- COMMAND --` | Save / quit (`:w`, `:q`, ...)           |

ESC always returns to NORMAL.  The status bar at the bottom row of
the screen also shows the filename, a `[+]` flag if the buffer has
unsaved changes, and `line:col`.

## Normal-mode keys

| Key             | What it does                                          |
|-----------------|-------------------------------------------------------|
| `h` / left      | cursor left                                           |
| `l` / right     | cursor right                                          |
| `k` / up        | line up                                               |
| `j` / down      | line down                                             |
| `0`             | jump to start of line                                 |
| `$`             | jump to end of line                                   |
| `gg`            | top of file                                           |
| `G`             | bottom of file                                        |
| `i`             | INSERT at cursor                                      |
| `a`             | INSERT one column past the cursor                     |
| `I`             | INSERT at start of line                               |
| `A`             | INSERT at end of line                                 |
| `o`             | open a new line below; INSERT on it                   |
| `O`             | open a new line above; INSERT on it                   |
| `x`             | delete the character under the cursor                 |
| `dd`            | delete the current line                               |
| `:`             | enter COMMAND mode                                    |

## Command mode

After typing `:`, your input shows in the status bar.  Hit Enter to
execute, ESC or Enter on an empty line to cancel.

The byte following ESC is re-dispatched as a fresh keypress -- so
the very common sequence `ESC :wq` (return to NORMAL, then enter
COMMAND mode) works as expected.  Older revisions of this editor
silently swallowed the byte after a lone ESC, which made `ESC :` a
no-op.  If you hit that, you're on a pre-fall-through build -- two
ESCs in a row is a workaround.

| Command | What it does                                  |
|---------|-----------------------------------------------|
| `w`     | write the buffer to the file (`saved N bytes`) |
| `q`     | quit (refuses if dirty -- use `q!`)            |
| `q!`    | force-quit, discard changes                    |
| `wq`    | write + quit                                   |
| `x`     | same as `wq`                                   |

## Limits

- Buffer is **64 lines x 120 chars** (same as `ked`).
- Display assumes a **24x80 VT100-style terminal**; we don't query
  the actual size, just clear the screen and position glyphs at
  `(row, col)` with `ESC [ R ; C H`.  Wider/taller terminals show
  extra empty space; narrower ones wrap visually.
- No undo, no `/` search, no yank/paste, no movement repeats (no
  `3w`, `10dd`, etc.).  Plenty of follow-ups.

## When to use `vi` vs `ked`

- `ked` is fine for one or two-line edits over a remote serial
  console that doesn't have a working VT100 — it never repaints, you
  always type a command, you always see the next prompt.
- `vi` needs a proper terminal but is much faster for any edit longer
  than "tweak line 5".

If you're on the QEMU `-serial stdio` rig, both work — the host
terminal speaks VT100.

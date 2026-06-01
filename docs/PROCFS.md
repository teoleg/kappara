# procfs and /dev — introspection

Everything in `/proc` and `/dev` is a **STREAMS character device**.
There is no "procfs" filesystem in the Linux sense — each entry is a
chrdev whose driver's `qi_qopen` formats a snapshot into the read
queue.  `cat /proc/ps` works because the qopen primes the queue with
text and the snapshot is followed by an `M_HANGUP` so the next
`sys_read` returns 0 cleanly.

The same pattern powers `/dev/klog`: opening it replays the boot log.

## /dev — drivers

| Path           | Major | Description                                   |
|----------------|-------|-----------------------------------------------|
| `/dev/loop`    | 1     | Echo: writes come back on reads               |
| `/dev/null`    | 2     | Sink: writes drop, reads return 0             |
| `/dev/console` | 3     | UART TX + RX through the stream head          |
| `/dev/klog`    | 4     | Kernel log buffer (replay on open)            |
| `/dev/fbcon`   | 7     | Framebuffer text console (write-only)         |

`lsl /dev` shows the major,minor tuple:

```
kappara:/# lsl /dev
chr    7,  0 fbcon
chr    4,  0 klog
chr    3,  0 console
chr    2,  0 null
chr    1,  0 loop
```

The major number IS the index into `cdevsw[]`.

## /proc — kernel state

All read-only.  Each `cat /proc/X` produces a fresh snapshot.

### /proc/ps

```
kappara:/# cat /proc/ps
  TID  STATE   NAME
    0  READY   main
    1  READY   uart_rx
    2  RUN     user-init
    3  BLOCK   spawn
```

Columns:
- **TID** — thread id, monotonic
- **STATE** — `READY`/`RUN`/`BLOCK`/`DEAD` (= KT_* enum)
- **NAME** — thread name passed to `kthread_create`

DEAD threads stay visible until the next `switch_to_next` runs the
reap path; usually you don't see them at all.

### /proc/meminfo

```
kappara:/# cat /proc/meminfo
pmm_free_pages:    243957
pmm_free_KiB:      975828
slab_objs_used:        58
slab_objs_free:       432
slab_bytes_tot:     19968
```

- `pmm_free_*` — the page-frame freelist (kfree/pmm_free return here)
- `slab_*` — totals across all `kmalloc` size caches

### /proc/slabinfo

```
kappara:/# cat /proc/slabinfo
NAME        OBJSIZE   FREE  TOTAL
size-16          16    254    254
size-32          32     96    127
size-64          64     48     63
size-128        128     21     31
size-256        256     13     15
size-512        512      0      0
size-1024      1024      0      0
size-2048      2048      0      0
```

One row per power-of-two size cache.  `OBJSIZE` is the per-object
size, `FREE`/`TOTAL` count slab slots currently free vs allocated to
the cache.

Use this to spot leaks: snapshot before + after a workload and look
for `FREE` going down without `TOTAL` going up.

### /proc/streams

Two sections.  First the static registry (anything `streams_register`'d
at boot); second the **live** stdata list, one row per open stream,
showing refcount + the module name walk from the stream head down.

```
kappara:/# cat /proc/streams
registered modules/drivers:
  fbcon
  delay
  upper
  klog
  console
  null
  loop

open streams:
  console    refs= 1  stack: strhead -> console
  loop       refs= 1  stack: strhead -> upper -> loop
  pipe-a     refs= 2  stack: strhead  [pipe]
```

The `[pipe]` tag marks the two ends of a `sys_pipe`; their write side
points at the peer's read queue instead of a driver, so the stack
walk stops after the head.  `[EOF]` shows up when `SD_EOF` is set
(peer closed; M_HANGUP was processed).

You can correlate this with `lsl /dev` and `lsl /proc` — every chrdev
inode under those is *potentially* an open here, but only the ones
someone has called `sys_open` on (or which the pipe constructor
built) show up.

## How to add a new /proc entry

Three steps:

1. **Claim a major** in `include/kappara/cdevsw.h` (`CDEV_MAJ_PROC_*`).
2. **Write the snapshot generator** in `kernel/proc.c`.  Pattern:
   ```c
   static struct procbuf foo_pb;

   static int proc_foo_qopen(queue_t *q)
   {
       pb_reset(&foo_pb);
       pb_str(&foo_pb, "header\n");
       /* ... append rows ... */
       pb_flush_to_q(&foo_pb, q);   /* also sends M_HANGUP */
       return 0;
   }

   PROC_DRIVER(foo, proc_foo_qopen);
   ```
3. **Register** in `proc_init`:
   ```c
   cdev_register(CDEV_MAJ_PROC_FOO, "proc-foo", &foo_streamtab);
   vfs_mknod_chrdev(proc, "foo", MKDEV(CDEV_MAJ_PROC_FOO, 0));
   ```

`pb_flush_to_q` automatically appends an `M_HANGUP` message so `cat`
exits after the snapshot is drained.

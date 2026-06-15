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
  TID  STATE  PRI  CL   NAME
    0  RUN      -  SYS  main
    1  RUN     60  SYS  uart_rx
    2  RUN     30  TS   user-init
    3  RUN      -  SYS  idle
    4  RUN      -  SYS  idle
    5  RUN      -  SYS  idle
    9  RUN     30  TS   ps
```

Columns:
- **TID** — thread id, monotonic
- **STATE** — `READY`/`RUN`/`BLOCK`/`DEAD` (= KT_* enum)
- **PRI** — dispatch priority.  Higher = picked first.  Idle threads
  show ` - ` (they carry `KSCHED_PRI_IDLE = -1` as a marker; they're
  never enqueued, so the number wouldn't mean anything).  SYS threads
  sit at `KSCHED_PRI_SYS_DEFAULT = 60`; TS threads start at
  `KSCHED_PRI_TS_DEFAULT = 30` and drift downward as `cl_tick`
  demotes them on quantum expiry (see ARCHITECTURE.md scheduling).
- **CL** — scheduling class: `SYS` (kernel threads, fixed priority)
  or `TS` (EL0 user threads, priority ages with CPU consumption).
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

### /proc/ftrace

Per-CPU function tracer; see `docs/FTRACE.md` for the full story.
Unlike the rest of `/proc` this entry also accepts **writes** — an
ASCII verb (`on`, `off`, `reset`) flips the tracer's runtime switch:

```
kappara:/# ftrace off
ftrace: off -> 3
kappara:/# ftrace dump          # alias for `cat /proc/ftrace`
ftrace: disabled  ring_per_cpu=256
[cpu 0] events=538004 (dropped=537748) shown=256
[250535828] e stream_write  <- sys_write_impl+0x7c
...
```

When the kernel is built without `TRACE=1`, the ring is permanently
empty and the verbs are no-ops; the entry still exists.

### /proc/cpuload

One row per CPU.  Snapshot of the scheduler's per-CPU dispatcher
state: who's running right now, whether the CPU is on its idle
thread, and how many runnable threads are waiting in that CPU's
dispatch queue.  Useful for watching the push-side load balancer
do its thing.

```
kappara:/# spawn
kappara:/# spawn
kappara:/# spawn
kappara:/# cat /proc/cpuload
CPU  STATE  DISPQ  CURRENT
  0  BUSY       0  user-init
  1  BUSY       0  spawn
  2  BUSY       0  spawn
  3  BUSY       0  spawn
```

`STATE = IDLE` means that core is parked on its `cpu_idle` thread
(WFI'd, waiting for an IPI or a tick).  `DISPQ` is the length of
the CPU's per-CPU dispatch queue at sample time — non-zero only
when more runnable threads exist than cores.  Single-word reads,
no locks — the sample may be stale by one instruction, which is
fine for a diagnostic.

### /proc/netif

One row per registered network interface (`netif_for_each` walk of
the registry).  Columns: name, flags, MTU, IPv4 address, netmask.
The user-facing tool is `cmd/ifconfig`, which just reads this file
and dumps it verbatim.  Real Solaris `ifconfig -a` shows much more
(zone, multicast, broadcast, error counters); we'll add columns when
we add the kernel state to back them.

```
kappara:/# cat /proc/netif
NAME    FLAGS  MTU     IP              NETMASK
lo0     UP      1500  127.0.0.1       255.0.0.0
```

Read-only.  There's no `ifconfig lo0 up` because every netif is
statically registered at boot — nothing to mutate.  When SLIP and
Ethernet drivers land with runtime-configurable IP/MTU, write-side
ioctls (or a `/dev/dlpi`-shaped control stream) become the place to
add mutation.

### /proc/tcp

One row per registered TCP TCB (`tcp_for_each_tcb` walk).  Columns:
state, local port, peer (ip:port or `-` for unconnected), send-buffer
length, send window (`swnd`, peer's last advertised receive window),
receive window (`rwnd`, what we'd advertise right now), smoothed
RTT, current RTO.  Listener TCBs with queued pending SYNs append
`backlog=N` -- the depth of the multi-accept ring
(`TCP_LISTEN_BACKLOG`, currently 8).

```
kappara:/# cat /proc/tcp
STATE         LPORT  PEER             EXTRAS
LISTEN         24680  -                sbuf=0  swnd=0 rwnd=8192  srtt=0ms rto=0ms backlog=2
ESTABLISHED    49152  127.0.0.1:24680  sbuf=0  swnd=8192 rwnd=8192  srtt=10ms rto=40ms
```

The user-facing tool is `cmd/netstat`, which concatenates
`/proc/netif`, `/proc/slip`, and `/proc/tcp`.

### /proc/slip

Per-line counters for `slip0`: total UART bytes received, plus SLIP
framing counters (rx_frames, rx_runts, rx_overflow, tx_frames).

## How to add a new /proc entry

Three steps:

1. **Claim a major** in `include/kappara/cdevsw.h` (`CDEV_MAJ_PROC_*`).
2. **Write the snapshot generator** in `uts/os/proc.c`.  Pattern:
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

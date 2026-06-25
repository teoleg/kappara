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
NAME           OBJSZ   USED   FREE  TOTAL  SLABS    KB
size-16          16      0    254    254      1     4
size-32          32     88     39    127      1     4
size-64          64     40     23     63      1     4
size-128        128     54      8     62      2     8
size-256        256     11      4     15      1     4
size-512        512     18      3     21      3    12
size-1024      1024      5      1      6      2     8
size-2048      2048      0      0      0      0     0
mblk             56      1     71     72      1     4
dblk             24      1    168    169      1     4
queue           104      0      0      0      0     0
                                                     13    52  (totals)
```

One row per slab cache:
- The size-N buckets back `kmalloc(N')` for any `N' <= N`.
- Per-subsystem named caches (today: STREAMS `mblk` / `dblk` /
  `queue`) appear below the size buckets if they've been
  `kmem_cache_init`'d -- the registry picks them up at init.

Columns:
- `OBJSZ`  -- per-object size after alignment rounding.
- `USED`   -- objects currently handed out (`total - free`).
- `FREE`   -- objects sitting on the cache's freelist.
- `TOTAL`  -- objects carved across all backing slab pages.
- `SLABS`  -- distinct 4 KB pages the cache has pulled from
              PMM; that's the memory committed to this cache.
- `KB`     -- `SLABS * 4`, restating the commitment as KiB.

The trailing `(totals)` line sums `SLABS` and `KB` across every
cache so you can read peak slab-allocator memory pressure at a
glance.

Use this to spot leaks: snapshot before + after a workload and
watch `USED` (without a matching drop on shutdown), `SLABS`, or
the totals climb.

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
receive window (`rwnd`, what we'd advertise right now), congestion
window (`cwnd`, RFC 5681 in-flight cap), slow-start threshold
(`ssthresh`), smoothed RTT, current RTO.  Listener TCBs with queued
pending SYNs append `backlog=N` -- the depth of the multi-accept
ring (`TCP_LISTEN_BACKLOG`, currently 8).  TCBs partway through fast
recovery append `dupacks=N` -- the running count of consecutive
duplicate ACKs (resets to 0 on the first new ACK).  When RFC 7323
window scaling is negotiated, the row appends `wscale=N` -- peer's
shift factor (`s->snd_wnd_shift`); we always advertise 0.

State names match the RFC 793 graph:

| State          | Meaning                                              |
|----------------|------------------------------------------------------|
| `CLOSED`       | TCB allocated, nothing bound yet                     |
| `BOUND`        | `T_BIND_REQ` succeeded, no `T_LISTEN_REQ` yet        |
| `LISTEN`       | accepting inbound SYNs into the backlog ring         |
| `SYN_SENT`     | active opener; sent SYN, waiting for SYN-ACK         |
| `SYN_RECEIVED` | passive opener; sent SYN-ACK, waiting for ACK        |
| `ESTABLISHED`  | both sides have agreed on ISS+1; data flows          |
| `FIN_WAIT_1`   | sent our FIN; awaiting ACK and/or peer FIN           |
| `FIN_WAIT_2`   | our FIN ACK'd; awaiting peer's FIN                   |
| `CLOSE_WAIT`   | peer FIN'd us; user hasn't sent `T_ORDREL_REQ` yet   |
| `CLOSING`      | simultaneous close; our FIN crossed peer's FIN       |
| `LAST_ACK`     | passive-closer sent FIN; awaiting peer's final ACK   |
| `TIME_WAIT`    | active-closer 2*MSL sweep; absorbs FIN retransmits   |

```
kappara:/# cat /proc/tcp
STATE         LPORT  PEER             EXTRAS
LISTEN         24680  -                sbuf=0  swnd=0 rwnd=8192  cwnd=1072 ssthresh=65535  srtt=0ms rto=0ms backlog=2
ESTABLISHED    49152  127.0.0.1:24680  sbuf=0  swnd=8192 rwnd=8192  cwnd=2680 ssthresh=65535  srtt=10ms rto=40ms
```

The user-facing tool is `cmd/netstat`, which concatenates
`/proc/netif`, `/proc/slip`, and `/proc/tcp`.

### /proc/slip

Per-line counters for `slip0`: total UART bytes received, plus SLIP
framing counters (rx_frames, rx_runts, rx_overflow, tx_frames).

### /proc/acpi

Summary of what `acpi_init()` (AWS.md stage C) pulled out of the
ACPI static tables.  Empty / "not present" line when booted via
`-kernel` (no EFI Configuration Table to walk).

```
kappara:/# cat /proc/acpi
rsdp:        0x4c760018
gic_version: 3
gicd_base:   0x8000000
gicr_base:   0x80a0000  len 0xf60000
cpus:        1
  cpu0 mpidr 0x0
pcie_ecam:   0x4010000000  bus 0..255
timer_ns_el1_gsiv:   30
timer_virt_el1_gsiv: 27
fadt_flags:  0x100000
```

Fields come from the MADT (GIC + per-CPU), MCFG (PCIe ECAM), GTDT
(timer GSIVs), and FADT (flags) tables.  Useful for confirming we
agree with the firmware before stage D's `pcie_init` consumes the
ECAM base.

### /proc/pci

PCIe device list produced by `pcie_init()` (AWS.md stage D) walking
the ECAM window.  Each row covers one (bus, dev, fn) endpoint.

```
kappara:/# cat /proc/pci
BDF      VID:DID    CLASS  HDR  MSI-X  BARs
00:00.0  1b36:0008  0600   00   -
00:01.0  1af4:1001  0100   00   0x98   00000001 10000000 0000000c 00000080
```

Columns:
- **BDF**: bus:device.function
- **VID:DID**: vendor / device ID.  Amazon vendor 0x1d0f's ENA
  (0xec20) and NVMe (0x8061) are called out by name in the boot
  log; here they're shown by raw hex.
- **CLASS**: class+subclass (0x0600 = host bridge, 0x0100 = mass
  storage SCSI controller, ...).
- **HDR**: header type (00 = endpoint, 01 = bridge).
- **MSI-X**: capability offset (or `-` if the cap isn't present).
- **BARs**: raw BAR values (unsized -- sizing requires a write/read
  cycle that's the driver's job).

Empty when MCFG isn't present (no UEFI / no ACPI).

### /proc/efi

EFI memory map snapshot captured by `efi_main` before
`ExitBootServices` (AWS.md stage B).  One row per descriptor.

```
kappara:/# cat /proc/efi
TYPE          START              PAGES     END
Conventional  0x0000000040000000      128  0x0000000040080000
LoaderCode    0x0000000040080000     1925  0x0000000040805000
Conventional  0x0000000040805000    14331  0x0000000044000000
BootData      0x0000000044000000       32  0x0000000044020000
...
ACPIReclaim   0x000000004c760000       16  0x000000004c770000
```

Type names follow the UEFI spec (Reserved, LoaderCode/Data,
BootCode/Data, RuntimeCode/Data, Conventional, Unusable,
ACPIReclaim, ACPINVS, MMIO, MMIOPort, PalCode, Persistent);
unknown types print as `?`.  Empty when booted via `-kernel`.

`ACPIReclaim` is where the firmware-installed static tables live
(RSDP / XSDT / MADT / ...); the kernel must NOT hand those pages
to the PMM until acpi_init has finished walking them.  Today the
PMM starts above `__kernel_end` so the question doesn't arise;
when stage D consumes the EFI memory map directly it'll filter
this type explicitly.

### /proc/nvme

NVMe controller summary populated by `nvme_init()` (AWS.md stage F)
after Identify Controller + Identify Namespace land.  Useful for
confirming the model + namespace size match what we booted with.

```
kappara:/# cat /proc/nvme
version:     1.4.0
bar0:        0x8000000000
vid:         0x1b36
model:       QEMU NVMe Ctrl
serial:      kapparanvme
firmware:    8.2.2
ns1_blocks:  32768
ns1_lba:     512 bytes
ns1_size:    16 MB
```

Empty / "no controller" line when no PCI device with class
0x0108 (Mass Storage / NVM controller) was found -- the usual
`-kernel` case.

### /proc/mounts

Live kfs mount table.  One row per mounted block_device with its
mountpoint path and filesystem type.  Backs the `mount` userspace
command.

```
kappara:/# mount
DEVICE     MOUNTPOINT     FSTYPE
ramdisk0  on  /usr/bin  type kfs
nvme0n1   on  /home     type kfs
```

`nvme0n1` only shows up under UEFI boot when an NVMe controller
is present (AWS.md stage F.1); the `-kernel` path falls back to
`ramdisk1` for `/home`.  Runtime `mount` / `umount` (taking
device + path arguments) isn't wired yet -- the kernel's
`kfs_mount` is only called from `exec_space_init` at boot.

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

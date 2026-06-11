# kappara architecture

A guide to the pieces and how they fit together.  Written in dependency
order, so each section assumes only the layers above it.

## Big picture

```
                +-----------------------------------------+
                |  user/init.c (ksh)                      |   EL0
                |  sys_open/read/write/spawn/kill/... ----|---svc #0--+
                +-----------------------------------------+           |
                                                                      v
                +-----------------------------------------+
                |  vectors.S  trap_dispatch  syscall.c    |   EL1 entry
                +-----------------------------------------+
                                  |
       +--------------------------+-------------------------+
       |                                                    |
   +-------+    +--------+    +-----------+    +---------+ |
   | sched |    | signal |    | streams + |    |  vfs +  | |
   | wait  |    | kill   |    |  STREAMS  |    |  inode  | |
   | yield |    | check  |    |  modules  |    |  vnode  | |
   +-------+    +--------+    +-----------+    +---------+ |
                                  |                  |     |
                                  v                  v     |
                            +-----------+     +---------+  |
                            | cdevsw[]  |     | kfs     |  |
                            +-----------+     +---------+  |
                                                 |        |
                                                 v        |
                                            +---------+   |
                                            |ramdisk  |   |
                                            +---------+   |
                                                          v
                +-----------------------------------------+
                |  pmm + kmem + mmu + timer + uart         |   the floor
                +-----------------------------------------+
```

## Boot

`uts/aarch64/boot.S` is the reset vector.  All four raspi3b cores
enter at `_start`.  Core 0 (MPIDR_EL1.Aff0 == 0) proceeds; cores 1-3
park in `.Lpark` at WFI (see commit `aa8759f` for why not WFE).

Core 0 walks the EL ladder.  raspi3b drops us at EL2, so we set
SPSR_EL2 = EL1h, ELR_EL2 = continuation, eret.  Now at EL1 with all
DAIF masked, we zero `.bss`, set up an initial stack, and `bl kmain`.

`kmain` (in `uts/os/main.c`) runs a fixed init sequence:

1. `uart_init` — PL011 ready for prints.
2. `trap_init` — `msr vbar_el1`.
3. `mmu_init` — build identity-map L0/L1/L2 tables, set TTBR0/TCR/MAIR,
   enable M+C+I in SCTLR.
4. `discover_gpu_reserve` — calls `framebuffer_init` (VC mailbox) so we
   know where the firmware put the framebuffer, then trims pmm's upper
   bound to leave that block alone.  Prevents the "boots in QEMU, dies
   on Pi" failure mode where kmalloc hands out a page the GPU also
   uses.
5. `pmm_init` — enrol [__kernel_end, gpu_trim) onto the freelist.
6. `kmem_init` — build the eight size-cache slabs.
7. `vfs_init` — root dentry/inode.
8. `streams_head_init` — register STREAMS modules + create `/dev`.
9. `proc_init` — register `/proc/*` cdevs.
10. `user_init` — copy the embedded user blob into the EL0 2 MB
    region, ic iallu, mmu_map_user_2mb.
11. `ramdisk_init` — zero the kfs backing store.  Must precede
    `exec_space_init` because the latter calls `kfs_mkimage` to
    populate `/usr/bin`; reversing the order silently wipes the
    fresh content.
12. `exec_space_init` — map the EL0 exec/stack/heap 2 MB windows,
    register `/bin/hello` (in-kernel blob), build a `kfs_payload`
    table from the cmd ELFs and call `kfs_mkimage` + `kfs_mount` to
    publish `/usr/bin` on the ramdisk.
13. Draw the splash on the framebuffer (one-time write; the kprintf
    tee that used to compete with QEMU's display thread is disabled
    by default — see commit `0cd91fd`).
14. `sched_init` (make `main` tid 0), `timer_init(100)`.
15. Spawn `uart_rx` and one `user-init-N` kthread per `/dev/ttyN`
    (4 shells total).  Each shell enters EL0 with x0 = N so its
    `_start` can open `/dev/ttyN` as fds 0/1/2.  `uart_rx_main`
    routes UART bytes only to the active tty, so the three
    background shells sit `BLOCKED` in `sys_read` until `Ctrl-X N`
    switches the active minor.
16. Drop into the idle loop: `kthread_yield(); wfi;` forever.

## Memory

### Physical (pmm)

A freelist of 4 KB pages.  Each free page stores the next pointer in
its first 8 bytes.  Alloc pops the head, frees push.  O(1) both ways,
no metadata table.

`pmm_init(start, end)` is told two boundaries:
- `start` = `__kernel_end` (linker symbol, page-aligned past BSS)
- `end` = `min(PLAT_RAM_END, framebuffer_base)` — the GPU reserve trim

`pmm_add_range(start, end)` enrolls an additional non-contiguous chunk
onto the freelist after `pmm_init`.  This is what carves the
user-mapped VA windows (`0x10000000..0x10200000` and
`0x20000000..0x20400000`) out of the kernel-stack pool: those PAs would
otherwise have their identity VAs silently aliased by the L2-entry
overwrites that `mmu_map_user_2mb` performs in `user_init` /
`exec_space_init`, and a kthread whose kernel stack landed in one of
those windows would find its saved-register frame trampled by EL0
writes to the same physical bytes.

The freelist + counter are guarded by a single `pmm_lock`.  Without
it, two CPUs racing in `pmm_alloc` could pop the same head and hand
out the same 4 KB page as two different kernel stacks — exactly the
class of corruption that produced the "saved callee-saved registers
full of `/proc/ps` text" instruction-abort panic before the lock
landed.

### Slab (kmem)

Eight power-of-two `kmem_cache`s: 16, 32, 64, 128, 256, 512, 1024,
2048 bytes.  Each cache holds a list of slabs (one slab = one 4 KB
page divided into N objects).  `kmalloc(n)` picks the smallest cache
that fits and pops an object; `kfree(p)` pushes it back.

The slab header lives at the very top of the page; a magic byte lets
`kfree` validate the pointer.

Memory recycling is **not zeroed** on alloc.  Callers that need clean
memory (struct kthread, sometimes new_inode) explicitly `kmemset`.
This is the bug from earlier in the project's history — see commit
`983e1c2` for why we now zero struct kthread.

A single coarse `kmem_lock` guards every cache's freelist and the
`grow_cache` slab-page install path.  Lock order is **kmem → pmm**:
`grow_cache` calls `pmm_alloc` while still holding `kmem_lock`; no
path acquires them in the reverse order, so deadlock is impossible.

### MMU

Single TTBR0 covering 0..1 GiB.  L0/L1/L2 with the L2 split into 2 MB
blocks.  Most blocks are Normal cacheable inner-shareable.  The
peripheral window (0x3F000000..0x40000000 on Pi 3) is Device-nGnRE.

`mmu_map_user_2mb(va, pa)` overrides one L2 block with user-accessible
attributes (AP[2:1] = 01).  That's the userspace 2 MB at VA
0x10000000.

### User address space layout

```
VA              Size    Purpose
0x10000000      2 MB    init binary (user_storage BSS + mmu_map_user_2mb)
                        Stack top = 0x10200000; spawned threads share this window
0x20000000      2 MB    exec code      (exec_storage[slot])
0x20200000      2 MB    exec stack     (exec_stack_storage[slot])
                        Stack top = 0x20400000
0x20400000      2 MB    exec heap      (exec_heap_storage[slot])
                        Grows up from EXEC_HEAP_VA, controlled by SYS_brk
```

R6: each exec'd process owns its own vm_map (L0/L1/L2/L3 page
tables) with EXEC_VA/EXEC_STACK_VA/EXEC_HEAP_VA mapped 4 KB at a
time onto PMM-allocated pages.  No fixed slot pool; the number of
concurrent processes is bounded only by PMM size (every exec
consumes roughly 2 MB of stack + a few KB of code + L3/L2/L1/L0
tables).  `sys_execve` allocates code pages while copying PT_LOAD
bytes and the full 2 MB stack range upfront.  `sys_fork` walks the
parent's L3 tables and allocates a fresh PMM page for each
already-mapped user page, kmemcpying parent->child before installing
the mapping in the child's L3.  Same EL0 VAs, different physical
pages -- real Unix fork semantics.  `vm_map_put` -> `mmu_vmap_destroy`
walks the L3 tables and `pmm_free`'s every user page on the last
reference.

`sys_brk` lives in the kernel's per-process `vm_map.heap_brk`.
Growing the break allocates PMM pages and installs 4 KB mappings;
shrinking unmaps + frees them.  malloc and friends just call
`brk(0)` / `brk(addr)` as before; the libc layer is unchanged.

The init window (USER_VA at 0x10000000) is still mapped once at
boot as a single shared 2 MB block (all four init shells share
`user_storage`).  fork is only available to exec'd processes; init
shells trying to fork get rejected.  Only one exec'd program runs at
a time per shell (the shell calls `sys_wait` before accepting the
next command).

### ELF loader (sys_execve)

`sys_execve_impl` in `uts/os/user.c`:

1. Opens the VFS path via `read_file_kernel` (bypasses the fd table so
   the 256 KB static `elf_read_buf` can be written directly without a
   `copy_to_user` round-trip).
2. Validates the ELF64 header: magic, ELF64 class, LE, `EM_AARCH64`,
   `ET_EXEC`.
3. Zeroes `exec_storage` and `exec_stack_storage` (BSS-clean start).
4. Copies each PT_LOAD segment from `elf_read_buf` into `exec_storage`
   at `p_vaddr - EXEC_VA`.  Segments outside `[0x20000000, 0x20200000)`
   are rejected.
5. `dsb ish; ic iallu; dsb ish; isb` — D→I cache coherence.
6. Builds the exec stack: argv strings are packed from `EXEC_STACK_TOP`
   downward (each NUL-terminated, 8-byte aligned), then the pointer array
   (`argv[0]..argv[argc-1]`, NULL terminator), then `argc` as a
   `uint64_t`.  SP is set to the `argc` word.  Because the pointer table
   is `(argc + 2)` words of 8 bytes each, an 8-byte padding word is
   inserted when `(argc + 2)` is odd to keep SP 16-byte aligned — the
   AArch64 ABI requirement at function-call boundaries.
7. Spawns an `exec` kthread with `kthread_inherit_fds` (so fd 0/1/2 are
   all `/dev/console`) and calls `aarch64_enter_userspace(e_entry, sp, 0)`.

Exec stack frame at entry (grows down from `EXEC_STACK_TOP = 0x20400000`):
```
[argv string data, NUL-terminated, 8-byte aligned, packed from top]
[optional 8-byte alignment pad if (argc+2) is odd]
[NULL pointer   — argv[argc]]
[argv[argc-1]   — user VA of last string]
…
[argv[0]        — user VA of program name]
[argc (uint64_t)] ← SP, 16-byte aligned
```
`crt0.S` reads `ldr x0, [sp]` (argc) and `add x1, sp, #8` (argv) then
calls `main(argc, argv)`.

Programs in `/bin` are ELF blobs incbin'd into the kernel image
(`uts/aarch64/helloblob.S`) and registered via `blob_fops` in the VFS.
Programs in `/usr/bin` are embedded via `uts/aarch64/usrblobs.S`
(source lives in `cmd/`) and copied at boot into the kfs ramdisk by
`exec_space_init` — at runtime `/usr/bin` is a kfs mount, not a
collection of in-memory inodes.  The exec loader doesn't care which
shape it's reading: `read_file_kernel` goes through `f_ops->read`,
which fans out to `blob_read` for `/bin` and `regfile_read` (through
the block device) for `/usr/bin`.

The shell opens `/dev/console` three times at startup to fill fds 0
(stdin), 1 (stdout), and 2 (stderr).  Exec'd programs inherit these so
`sys_write(1, ...)` works without the exec'd binary needing to open the
console itself.

Exec-space programs can call `sys_spawn` to create sub-threads.
`sys_spawn_impl` detects the entry-point range and chooses the correct
stack pool:

- Entry in `[0x10000000, 0x10200000)` → init-space stacks, counter
  `spawn_next`.
- Entry in `[0x20000000, 0x20200000)` → exec-space stacks from
  `exec_stack_storage`, counter `exec_spawn_next`.  Slot 1 gets
  SP = `0x203F0000`, slot 2 `0x203E0000`, etc.

`exec_spawn_next` is reset to 0 at the start of each `sys_execve` call
so every exec'd program starts with a fresh pool.  Convention: exec'd
programs must `sys_wait` for sub-threads before exiting to avoid a race
where the next `sys_execve` zeroes the exec stack storage while a stale
sub-thread is still running.

### lib/libc — freestanding C library

Source: `lib/libc/`.  Built as a static archive `build/cmd/libc.a` and
linked into every `/usr/bin` binary.  Uses `-ffreestanding -nostdlib`
so it has no host-OS dependencies.

| Module                  | What it provides                                             |
|-------------------------|--------------------------------------------------------------|
| `aarch64/crt0.S`        | `_start`: loads argc/argv from exec stack, calls `main`, then `sys_exit`. |
| `aarch64/internal.h`    | `__syscall1/__syscall3`, syscall numbers, `ssize_t`.          |
| `src/string.c`          | `strlen`, `strcpy`, `strncpy`, `memcpy`, `memset`, `strcmp`. |
| `src/printf.c`          | `printf`, `vprintf`, `sprintf`, `snprintf`, `vsnprintf`.      |
| `src/malloc.c`          | `malloc`, `free`, `calloc`, `realloc` — free-list allocator backed by `SYS_brk`.  `heap_grow()` calls `brk(0)` + `brk(cur+n)` (rounded up to 4 KB) to extend the heap on demand. |
| `src/file.c`            | `FILE*` layer: `fopen/fclose/fread/fwrite/fgets/fputs/fputc/fgetc`, `fprintf/vfprintf`, `puts/putchar`.  `stdin/stdout/stderr` backed by fd 0/1/2. |
| `src/io.c`              | `read`, `write`, `open`, `close`, `pipe`, `_exit`.            |
| `include/`              | `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<unistd.h>`, `<stddef.h>`, `<stdarg.h>`, `<sys/types.h>`. |

The malloc heap lives in a dedicated 2 MB user-VA window at
`EXEC_HEAP_VA = 0x20400000` mapped to `exec_heap_storage` in kernel
BSS.  `SYS_brk(addr)` advances the break within `[EXEC_HEAP_VA,
EXEC_HEAP_VA+2MB)`; `sys_execve_impl` resets the break to
`EXEC_HEAP_VA` and zeroes `exec_heap_storage` so every program starts
with a fresh heap.  Because the heap window's user VA is remapped to
the BSS PA, the matching kernel-VA range
`[0x20400000, 0x20600000)` is excluded from the PMM (see the
`EXEC_HOLE_END` carve-out in `main.c`).

`FILE*` wraps an `int fd` and delegates to the raw `read`/`write`
syscall wrappers in `io.c`.  It has no knowledge of STREAMS; the
STREAMS machinery is transparent at the syscall boundary.

## Scheduling

`struct kthread` lives in `include/kappara/sched.h`.  Each has:

- `sp` — saved kernel SP (resumed by `context_switch`)
- `stack_base` — the page allocated for the kernel stack
- `name`, `comm[32]` — display name for `/proc/ps` and `kprintf`.  `name` always points into the embedded `comm` field; `kthread_create` copies the caller's string in so the source can be a short-lived buffer (e.g. `sys_execve_impl`'s resolved program basename).
- `state` — READY / RUNNING / BLOCKED / DEAD
- `next` — link in whatever queue we're on (ready, wait queue, to_reap)
- `sig_pending`, `waiting_on` — signal bookkeeping
- `fdt[KT_FD_MAX]` — per-thread fd table (POSIX fork-style inherit)

The scheduler is round-robin FIFO.  `kthread_yield()` does:

```
prev = cur
next = ready_pop()
ready_push(prev)
prev->state = READY; next->state = RUNNING; cur = next
context_switch(&prev->sp, next->sp)
```

`context_switch` (in `uts/aarch64/switch.S`) saves callee-saved regs
plus DAIF, swaps SP, restores callee-saved + DAIF from the new
thread.  Per-thread DAIF is critical (commit `0929814`): without it, a
thread that slept with IRQs masked would resume into whoever's DAIF
state the waker happened to have.

### Wait queues (sleepq)

`kthread_sleep_on(wq)` marks BLOCKED, threads onto `wq->head`,
yields without re-queuing.  `kthread_wake_all(wq)` walks the queue,
marks each READY, pushes to ready queue.

Used by `stream_read` for blocking I/O, by signal delivery to
surgically unlink a sleeper from its queue, and by pipe close to wake
the peer's reader with EOF.

**SMP discipline (Solaris sleepq, Phase 5).**  `struct wait_queue`
carries a `sq_lock` that the sleeper holds **across `context_switch`'s
save phase**.  Mechanism: `kthread_sleep_on` acquires `sq_lock`,
links itself, then passes `&sq_lock` to `switch_to_next` as
`extra_release`.  `switch_to_next` stashes the pointer in
`cpu_pending_release_lock`; the **incoming** thread releases
`sq_lock` after `context_switch` returns, before releasing
`cpu_thread_lock`.

A waker therefore cannot acquire `sq_lock` until the sleeper's `sp`
has been committed by the save phase.  Without this, a waker that
ran between `wq->head = t` and `context_switch`'s `str x2,[x0]`
could move `t` to the dispq and let another CPU `try_steal` `t` with
its `sp` field still holding the previous value — the same
steal-mid-save race that Phase 2 closed for the yield path, just
reached via the wake side instead of `dispq_push`.

### Reaping

`kthread_exit` parks the dying thread on `to_reap`.  The next
`switch_to_next` runs the reap loop **after** the context_switch — so
we're on a different stack, safe to `pmm_free` the dying thread's
stack and `kfree` its struct.

**SMP discipline (Phase 5b).**  `kthread_exit` acquires
`to_reap_lock` and **does not release it** before calling
`switch_to_next(0, &to_reap_lock)` — the lock is carried across the
switch the same way sleepers carry `sq_lock`, via
`cpu_pending_release_lock` released by the incoming thread.  This
prevents a `to_reap` drain on another CPU (every `switch_to_next`
resumer drains the global list) from `pmm_free`'ing the dying
thread's stack page **while save phase is still writing to it**.
The freed page would otherwise be reallocated immediately by slab
or another stack alloc, corrupting either the freelist or the
register save area.

### Signal interlock (phase 6)

Every `struct kthread` carries a per-thread `sigwait_wq`.
`sys_sigsuspend_impl` takes `sigwait_wq.sq_lock` around BOTH the
mask install AND the `(sig_pending & ~sig_mask)` check, then sleeps
via `kthread_sleep_on_locked(&t->sigwait_wq, flags)` -- same
extra-release pattern phase 5 introduced for sq_lock and phase 5b
for to_reap_lock.

`kthread_signal` takes the target's `sigwait_wq.sq_lock` around the
`sig_pending |= SIGBIT(sig)` OR, so the check-then-sleep window
inside sigsuspend is closed: either the signaler sets the bit
before sigsuspend's check (which then sees it and returns without
sleeping), or sigsuspend gets sq_lock first and the signaler spins
until sigsuspend has slept -- at which point the signaler surgically
removes the thread from sigwait_wq under the same lock it already
holds.  A target NOT in sigsuspend pays a single uncontended
acquire (the wq is per-thread, nobody else touches its sq_lock).

If the target is parked on some OTHER wait queue (stream_read,
sys_wait, ...), the signaler then takes THAT queue's sq_lock and
does the existing surgical-remove + dispq_push.  Lock order is
`sigwait_wq.sq_lock` outer, peer wq sq_lock inner -- consistent
because nothing else acquires those two together.

## STREAMS

SVR4 vocabulary, intentionally.

### Message structure

```
   mblk_t (header)         dblk_t (data)
   +-------------+         +-----------+         +---------------+
   |  b_next     |         |  db_ref   |         |  buffer       |
   |  b_cont -----+        |  db_type  |         |               |
   |  b_rptr ------+-->----+           |         |               |
   |  b_wptr ------+       |  db_base ----------->               |
   |  b_datap ------------>+           |         |               |
   +-------------+         +-----------+         +---------------+
```

`b_cont` chains continuation pieces of the **same message**.  `b_next`
chains messages in a queue's deferred list.  `dupb` makes a new mblk
that shares the dblk (db_ref counts the sharing); `freeb` frees one
mblk, `freemsg` frees a whole chain.

### Queues

Each module/driver direction is a `queue_t`.  Modules expose a
`qinit` per direction with `qi_putp` (called when a message arrives)
and an optional `qi_srvp` (service procedure, called from
`streams_run`).

`putq(q, mp)` enqueues onto a queue's deferred list.  `getq(q)` pops.
`putnext(q, mp)` is `q->q_next->q_qinfo->qi_putp(q->q_next, mp)`.

### Stream head

The top of every open stream.  `sh_rq_putp` queues incoming messages
for `sys_read`; `sh_wq_putp` forwards downstream.  M_HANGUP from below
sets `SD_EOF` on the stdata.

### cdevsw

`cdevsw[major]` is the SVR4 character-device switch.  Each driver
registers via `cdev_register(major, name, streamtab*)`.  Inodes carry
a `dev_t` (encoded `(major<<24)|minor`); `vfs_lookup` + open uses
MAJOR(rdev) to find the streamtab and build a stream around it.  No
Linux-style "f_ops on every inode" — drivers live in cdevsw, not on
the inode.

### Multiplexors (I_LINK / I_UNLINK)

A STREAMS multiplexor is a driver that talks to **multiple lower
streams** as a single logical entity.  IP is the canonical example:
one IP driver, many netifs (lo0, slip0, eth0) joined under it, and
many upper protocols (UDP, ICMP, TCP) joined above it.

`ioctl(upper_fd, I_LINK, lower_fd)` joins the lower stream beneath
the upper stream's driver.  Mechanically:

1. Stream head allocates a muxid (1..MUX_MAX), records `upper` and
   `lower` in `muxtab[]`.
2. Builds an M_IOCTL{`I_LINK`} carrying a `struct linkblk { l_qtop,
   l_qbot, l_index }` -- `l_qbot` is the lower stream's head write
   queue, used by the mux driver later for "putnext into the lower
   stream's top".
3. `putnext`'s the M_IOCTL down the upper stream's write side.  The
   mux driver's `wput` recognises I_LINK, stores `l_qbot` and the
   muxid in its per-instance state, flips db_type to M_IOCACK and
   `putnext`s back up.
4. On ack, the stream head **rewires the lower stream's read-side
   chain**: the queue whose `q_next` was the lower head's `sd_rq`
   now points at the **upper driver's** `drv_rq`.  After this,
   data going up from the lower driver (via `putnext`) lands in the
   upper driver's `rput`, which can demux on protocol byte (IP) or
   whatever it tracks.

`I_UNLINK` is symmetric: M_IOCTL{I_UNLINK} to the upper driver, on
ack the saved `q_next` is restored.

The in-kernel API (`stream_ilink` / `stream_iunlink`, taking stdata
pointers directly) is what built-in mux drivers use; the ioctl path
just resolves fds and calls through.  `mux_selftest` (runs at boot)
exercises the full round trip with a tiny `mux_demo` driver as upper
and the existing `loop` driver as lower.

This is what lets us build the network stack as **real STREAMS
modules** -- IP is a multiplexor with lo0/slip0/eth0 linked under,
UDP/ICMP/TCP linked over -- rather than a direct-call protocol stack
sandwiched between STREAMS endpoints.

### Pipes

`sys_pipe` builds two pipe_end stdata and cross-wires
`a->sd_wq->q_next = b->sd_rq` (and vice versa).  Each end has
`sd_peer` pointing at the other.  Closing one end signals
`SD_EOF` on the peer and wakes its readers.

### Stream-read interlock (phase 7)

`stream_read` takes `sd_readwait.sq_lock` across the whole
`(getq + SD_EOF check + sleep_on)` window and hands the lock to
`kthread_sleep_on_locked`.  `sh_rq_putp` and the pipe-close path
take the same lock around `putq(sd_rq, mp)` and the
`sd_flags |= SD_EOF` mutation.  Same-shape fix as phases 5c
(`sys_wait`) and 6 (`sigsuspend`): a writer that arrives "just
before" the reader sleeps either grabs sq_lock first (in which
case the reader's later `getq` sees the data and never sleeps),
or spins until the reader is fully on the wq (in which case
`kthread_wake_all` after the writer's release surgically wakes it).

`sq_lock` also doubles as the queue mutation lock for `sd_rq` --
nothing else serialised concurrent `putq` / `getq` / `putbq`
calls.  `stream_read` releases sq_lock before copying to user
(no spinlock across uaccess) and re-acquires it briefly around
the leftover `putbq` on partial reads.

## VFS

In-memory dentry/inode tree.  Each `dentry` has name + parent +
sibling + child + inode; each `inode` has type + fops + i_private +
i_rdev + i_count (= vnode v_count).

The dentry layer is more BSD-flavored than the SVR4 vnode-only model
because we have a separate name-cache; but the **lifecycle** is pure
SVR4: `vfs_iget` bumps the count, `vfs_iput` drops it, and when the
last reference goes the FS's `vop_inactive` callback releases
i_private and the inode is freed.

`sys_open` does `vfs_iget(ino)` before assigning to the file;
`file_put` (last close) does `vfs_iput`.  An `unlink` between open
and close removes only the name; the inode hangs around until the
last close drops the count — matching Unix removed-but-open
semantics.

### Reference counting (`f_refs`, `i_count`)

Both counters are reached from multiple CPUs the moment two threads
share an fd: parent and exec'd / spawned child each touch `f_refs`,
and either of them dropping the last reference races the inode
`i_count` drop in `file_put`.  Concurrent `++` / `--` on a plain `int`
on different CPUs loses updates — the canonical SMP refcount bug
that surfaces as either a leak (count stays positive forever) or a
double free (two CPUs both think they did the last drop).

The fix is the universal Unix refcount idiom: a lock-free atomic
counter, the same way Solaris's `atomic_inc_uint` / `atomic_dec_uint_nv`,
Linux's `refcount_inc` / `refcount_dec_and_test`, and FreeBSD's
`refcount_acquire` / `refcount_release` all do it.

Our primitives live in `include/kappara/atomic.h`:

- `atomic_inc(int *p)` — increment with ACQUIRE (LDAXR / STLXR loop).
- `atomic_dec_and_test(int *p)` — decrement with RELEASE; returns 1 if
  the caller drove the count to zero (and is therefore the unique
  cleanup observer); emits a `dmb ishld` on that path so the
  subsequent cleanup reads see every other CPU's prior RELEASE drop.

Both are hand-rolled inline asm in the same style as
`include/kappara/spinlock.h` rather than C11 `__atomic_*` builtins.
Reason: AArch64 GCC ≥ 10 lowers `__atomic_*` to libgcc "outline
atomics" helpers that internally call `__getauxval` to pick between
LSE and LL/SC at runtime.  Freestanding kernels don't have glibc, and
dragging libgcc into the trust boundary for a 32-bit counter add is
the wrong trade.  The inline asm is one cache-line touch and three
instructions in the uncontended case, with no symbol dependency
outside the kernel.

The discipline: refcounts are ONLY touched through chokepoint helpers
(`file_get` / `file_put` / `vfs_iget` / `vfs_iput`).  Plain `++` /
`--` is a bug; CLAUDE.md's hot-bug list calls this out.  Initial
stores at struct-creation time are plain because no other CPU has
the pointer yet.

## kfs

Simple disk layout:

```
block 0   superblock (magic, num_files)
block 1   free-block bitmap
block 2   root dirent table
block 3+  data, subdir dirent tables
```

Each file gets `KFS_BLOCKS_PER_FILE` contiguous blocks pre-allocated
(32 blocks = 16 KB per file, sized for the cmd ELFs).  The bitmap
supports alloc/free, so `rm` actually returns blocks to the pool
(see commit `fb3f938`).

`kfs_mkimage` is data-driven: callers pass a `struct kfs_payload[]`
of `{name, data, size}` tuples and mkimage lays them out at root
level.  `exec_space_init` builds such a table from the cmd-ELF
blob symbols and mounts the result at `/usr/bin`.  Future on-disk
storage (S2 SD/EMMC) plugs in by swapping the `block_device` —
nothing else above the kfs layer changes.

## Networking

Pure SVR4 STREAMS: IP is a real multiplexor driver, each netif is a
leaf STREAMS driver `I_LINK`ed underneath, and the data path is
`putnext` end-to-end.  No direct calls between layers in the kernel
hot path -- everything is queue-to-queue.  Why? See the doc-trail in
`docs/ARCHITECTURE.md` history; short version is "if the protocol
stack isn't actually composable then there's no reason to use STREAMS
at all."

```
┌─────────────────────────────────────────────────────────┐
│ user: cmd/ping  (open /dev/icmp, write req, read rep)   │
├─────────────────────────────────────────────────────────┤
│ stream head                                              │
├─────────────────────────────────────────────────────────┤
│ icmp module  (uts/os/net/icmp.c, struct icmp_streamtab)  │
│   qopen: alloc state, auto-assign icmp_id, putnext       │
│          M_PROTO{IP_T_BIND_REQ, proto=1} down            │
│   wput:  M_DATA{icmp_ping_req}: stamp our icmp_id, arm  │
│          (id, seq) waiter, build ICMP header, putnext    │
│          M_PROTO{IP_T_SEND_REQ}+M_DATA(payload) down     │
│   rput:  M_PROTO{IP_T_BIND_ACK} → bound=1                │
│          M_DATA: filter by icmp_id; type=8 auto-reply    │
│          via putnext(WR(q), ...); type=0 match waiter,   │
│          putnext UP formatted icmp_ping_rep              │
│   qclose: putnext UNBIND_REQ down, free state            │
├─────────────────────────────────────────────────────────┤
│ IP multiplexor (uts/os/net/ipv4.c, struct ip_streamtab)  │
│   wput: M_IOCTL{I_LINK/I_UNLINK}: lower-side bookkeeping │
│         M_PROTO{IP_T_SEND_REQ}+M_DATA: build header,     │
│           route via netmask in ip_lowers[], putnext      │
│           into lower->qbot                               │
│         M_PROTO{IP_T_BIND_REQ/IP_T_UNBIND_REQ}: register │
│           (proto, key, upper_rq) in ip_uppers[]; ACK     │
│           back UP via putnext(OTHERQ)                    │
│   rput: validate IPv4, strip, demux via ip_uppers[];     │
│         multicast via dupmsg when N matches              │
│         (cross-stream delivery uses put(), not putnext)  │
├─────────────────────────────────────────────────────────┤
│ Netif leaf STREAMS drivers (one stream each):           │
│   lo0 (loopback): wput putnexts OTHERQ(q) -- the I_LINK  │
│         rewire points its q_next at IP's drv_rq          │
│   slip0 (UART, future)                                  │
└─────────────────────────────────────────────────────────┘
```

### IP M_PROTO primitives

Every inter-layer interaction between an upper module and the IP
driver is an M_PROTO message tagged by a `prim` byte at offset 0
of the M_PROTO mblk's b_rptr (the SVR4 TPI / DLPI shape).  IP
recognises:

| Primitive            | Direction       | Carries                                    |
|----------------------|-----------------|---------------------------------------------|
| `IP_T_SEND_REQ`      | upper → IP      | `ip_send_meta{dst_ip, proto}` + M_DATA b_cont |
| `IP_T_BIND_REQ`      | upper → IP      | `ip_bind_meta{proto, key}`                 |
| `IP_T_BIND_ACK`      | IP → upper      | `ip_bind_meta{proto, key}` (matches request) |
| `IP_T_BIND_NAK`      | IP → upper      | `ip_bind_meta{proto, key}` (slot full / dup) |
| `IP_T_UNBIND_REQ`    | upper → IP      | `ip_bind_meta{proto, key}`                 |
| `IP_T_UNBIND_ACK`    | IP → upper      | `ip_bind_meta{proto, key}`                 |
| `IP_T_UNITDATA_IND`  | IP → upper      | `ip_unitdata_ind{proto, src_ip, dst_ip}` + M_DATA b_cont (the demuxed payload after IP-header strip) |

The `key` is a protocol-specific demux discriminator that IP
itself doesn't interpret -- ICMP leaves it 0 (binding by proto
alone); UDP will populate it with the local port.

### Multi-bind multicast demux

`ip_uppers[]` is a fixed-size table of up to 16 active bindings.
On incoming packet, IP's rput walks the table looking for entries
matching the packet's proto byte and collects all matches; then
`dupmsg` for all but the last delivery and `put(upper_rq, mp)` to
each match.  Cross-stream delivery uses `put` (the SVR4 primitive
that invokes a queue's own `qi_putp` directly without following
`q_next`) because the rput's source stream and the upper bind's
stream are not in the same `q_next` chain -- `putnext` would walk
the wrong way.

Each ICMP module instance gets a 16-bit `icmp_id` auto-assigned
at `qopen` time from a global counter (skipping zero).  Outgoing
echo requests are stamped with this id; incoming demuxed packets
that arrive at the module's `rq_putp` are filtered by id and
dropped if they don't match.  Two concurrent `cmd/ping`s get
distinct ids, each filters by its own, both work.  Same shape
Solaris uses for raw-ICMP endpoints.

### Stream tear-down for STREAMS modules

`stream_close` (and `stream_destroy_kernel`) walk pushed modules
top-down and call each one's `qi_qclose` on its **read-side**
qinit before draining and freeing the queues.  SVR4 convention.
The icmp module's `qclose` is what fires the `IP_T_UNBIND_REQ`
that releases the bind slot in IP -- without that call, sequential
opens of `/dev/icmp` would silently leak `ip_uppers[]` entries and
hit the duplicate-bind check.

### `/dev/icmp` autopush

`/dev/icmp`'s cdev entry maps to `ip_streamtab`.  On open, `stream_open`
detects `MAJOR(rdev) == CDEV_MAJ_ICMP` and `do_ipush`es the `"icmp"`
module on top of the freshly-built IP stream.  Same mechanism as
`ldterm` autopush on `/dev/tty*`.  The icmp module's qopen runs after
the push completes (all q_next wiring is in place) so its first
`putnext(WR(rq), bind_mp)` reaches the IP wput cleanly.

### Boot wiring

`ip_init()` runs from `main.c` *after* `sched_init` (not from
`streams_head_init`) because `stream_ilink` blocks on `sd_ioc_wq`
which derefs `curthread`.  It:
1. Calls `lo_init()` to register the lo0 netif in the registry.
2. Builds the kernel-only IP control stream `ip_ctl_sd`.  All
   in-kernel `ip_send(...)` calls go through this stream; users
   open separate `/dev/icmp` streams that get demuxed cross-stream.
3. Walks `netif_for_each`, builds one kernel stream per netif from
   its `streamtab`, `stream_ilink`s each under `ip_ctl_sd`, and
   calls `ip_lower_register(muxid, nif)` so the netif identity is
   available for routing.

### UDP module (`uts/os/net/udp.c`)

Same shape as ICMP: a pushable STREAMS module, not a driver.
`/dev/udp`'s cdev entry maps to `ip_streamtab`; `stream_open`
autopushes `"udp"` on top.  Per-open state lives in `struct
udp_state` (local_port, bound flag).

The user ABI is the SVR4 TPI primitive set, delivered via
`putmsg` / `getmsg`:

| Primitive       | Direction       | Carries                                |
|-----------------|-----------------|----------------------------------------|
| `T_BIND_REQ`    | user → kernel   | `t_bind_req{port}` (port=0 → ephemeral) |
| `T_BIND_ACK`    | kernel → user   | `t_bind_ack{port}` (actual port bound) |
| `T_BIND_NAK`    | kernel → user   | `t_bind_nak{reason}`                   |
| `T_UNITDATA_REQ`| user → kernel   | `t_unitdata_req{dst_ip,dst_port}` + M_DATA payload |
| `T_UNITDATA_IND`| kernel → user   | `t_unitdata_ind{src_ip,src_port}` + M_DATA payload |

UDP's wput on `T_BIND_REQ` records `local_port` and sends
M_PROTO{IP_T_BIND_REQ, proto=17, key=local_port} down to IP.  IP
records the binding in `ip_uppers[]` (proto=17, key=port).  The
`key` is the protocol-specific demux discriminator; IP itself does
not look at the UDP header to extract the port -- it just keeps
the binding indexed by (proto, key, upper_rq).  Two UDP modules
trying to bind the same port collide on IP's duplicate-bind check
and the second gets a NAK.

UDP's wput on `T_UNITDATA_REQ` validates the bind, copies the
M_DATA payload into a fresh mblk with `IP_HDR_LEN + UDP_HDR_LEN`
headroom, prepends the UDP header (`src_port = local_port`,
`dst_port` and length from the request, checksum 0), wraps in
M_PROTO{IP_T_SEND_REQ, proto=17, dst_ip}, and `putnext`s down to
IP.  IP prepends the IP header in the further-reserved headroom
and routes via netmask.

UDP's rput receives demuxed M_DATA from IP (via cross-stream
`put`), parses the UDP header, **filters by dst_port** matching
`local_port`, strips the UDP header, and wraps the body in
M_PROTO{T_UNITDATA_IND, src_ip, src_port} for `putnext` UP to the
stream head.  The filter is here (not at IP demux) because IP
multicasts to all proto=17 bindings regardless of key -- key is
only consulted for the duplicate-bind check at bind time.  Real
SVR4 IP does the same; UDP filtering at the transport is correct
encapsulation.

### Device major assignments (network):

| Major | Name  | Source              | Purpose                                  |
|-------|-------|---------------------|------------------------------------------|
| 16    | icmp  | `ip_streamtab` + autopush `"icmp"` module | ICMP raw-echo endpoint     |
| 18    | udp   | `ip_streamtab` + autopush `"udp"` module  | UDP TPI datagram endpoint  |
| 19    | tcp   | `ip_streamtab` + autopush `"tcp"` module  | TCP TPI connection endpoint (T1a: bind only) |

### TCP -- planned in phases (currently T1a)

Same architectural shape as UDP: a STREAMS module above IP with a
TPI user ABI delivered via putmsg/getmsg.  The `key` field of
`ip_bind_meta` packs `(local_port << 16) | remote_port` so IP's
exact-match demux delivers a segment straight to the right TCP
endpoint, no multicast needed once T1b lights up.

TPI primitives (locked in across phases; see
`include/kappara/net/tcp.h`):

| Phase | Status | Primitives                                                       |
|-------|--------|------------------------------------------------------------------|
| T1a   | ✓ done | T_BIND_REQ / T_BIND_ACK / T_BIND_NAK (via IP_T_BIND_REQ)         |
| T1b   | ✓ done | T_CONN_REQ / T_CONN_CON + 3-way handshake (active + passive)     |
| T1c   | ✓ done | T_DATA_REQ / T_DATA_IND (in-order seq/ack; no retransmit yet)    |
| T1d   |        | T_CONN_IND / T_CONN_RES (proper LISTEN + accept queue)           |
| T1e   |        | T_ORDREL_REQ / T_ORDREL_IND (FIN handshake) + T_DISCON_REQ/IND   |
| T1f   |        | retransmit timer + RTT estimation                                |
| T1g   |        | cmd/tcptest end-to-end                                           |

T1b currently uses a simplified single-connection listener: a TCB in
BOUND state accepts the first SYN inline, transitions to
SYN_RECEIVED, and morphs into the established connection on ACK.
T1d will split this properly into a LISTEN state with an accept
queue producing child TCBs per inbound connection.

Critical lo0 detail: state transitions are updated BEFORE the
synchronous `tcp_send_segment` call, not after.  lo0's tx loops
back into IP demux on the calling thread's stack, which re-enters
`tcp_rq_putp` with the response before the calling function has
returned.  If the state hasn't been updated by then, the recursive
callback sees stale state and drops the response.  Same shape ICMP
already had to work around in N2c (icmp_arm_waiter before
icmp_send_echo).

At T1a unimplemented primitives bounce back with
`T_DISCON_IND{reason=NOTSUP}` so users get a clear error rather
than silent drops.  The header is the locked-in ABI; subsequent
phases only fill in handler bodies.

(Major 15 was a leaf-driver `/dev/lo0` for raw-IP user injection.
Removed in N2b; the role belongs to a future `/dev/ip` once we
expose the IP mux to user space.)

### pktfilter -- the composability proof

`pktfilter` is a tiny STREAMS module shipped to demonstrate the
runtime-modular property the whole N2 rewrite was justified by.  It
is **not** part of any default stack; users I_PUSH it onto an open
TPI stream by hand:

```c
int fd = open("/dev/udp", O_RDWR);
ioctl(fd, I_PUSH, (long)"pktfilter");
/* ... configure via PF_T_SET_REQ M_PROTO ... */
/* ... send + see counters increment ... */
ioctl(fd, I_POP, 0);
```

After the push the stack is `head -> pktfilter -> udp -> ip`.
pktfilter's wput inspects M_PROTO mblks heading down:

- `T_UNITDATA_REQ` is matched against `(enabled, drop_dst_ip,
  drop_dst_port)`.  Matching messages are freed (counted as
  `dropped`).  Non-matching pass through to UDP unchanged
  (counted as `passed`).
- `PF_T_SET_REQ` / `PF_T_STATS_REQ` are pktfilter's own control
  primitives -- consumed at the module, replies (`PF_T_SET_ACK`
  / `PF_T_STATS_RES`) putnext UP to the head.
- Everything else (`T_BIND_REQ`, `M_DATA`, `M_IOCTL`, ...) is
  passed through unchanged so the bind/unbind handshake and any
  other unrelated control traffic still reach UDP.

pktfilter's rput is pure passthrough.  Inbound packets reach UDP
via IP's demux `put()` directly into UDP's rq, bypassing pktfilter
entirely -- a deliberate consequence of the multiplexor model.
Inbound filtering belongs at a different hook point (a future
`ip_hook_input` or a pktfilter pushed on `ip_ctl_sd` itself).

`cmd/pktfilttest` exercises the full push-configure-drop-pop cycle
and is the regression test that proves: a third-party STREAMS
module written long after the stack was built can slot in at
runtime, observe and modify outbound traffic, expose its own
control protocol via M_PROTO, and unwind cleanly via I_POP.  That
property is what justifies the per-packet putnext cost.

### SLIP -- off-host connectivity over the mini-UART

SLIP follows the SVR4 / Solaris `slattach` shape: a STREAMS module
pushed on top of a raw serial port, and the resulting stream is the
netif from IP's POV after `I_LINK`.  Three pieces in
`uts/os/net/slip.c`:

1. `miniuart_streamtab` -- a leaf STREAMS driver on the BCM2837
   mini-UART hardware (`uts/aarch64/miniuart.c`).  wput emits each
   M_DATA byte via `miniuart_tx_byte()`; rput is a passthrough that
   the rx kthread feeds.

2. `slip_streamtab` -- the SLIP framing module:
   - `wput` accepts an outbound IP packet (M_DATA from IP via I_LINK),
     byte-stuffs per RFC 1055 (END=0xC0, ESC=0xDB, ESC_END=0xDC,
     ESC_ESC=0xDD), prefixes and suffixes with END, putnexts down.
   - `rput` consumes byte-stream M_DATA from the mini-UART driver,
     runs the unstuffing state machine into a per-instance frame
     mblk (allocated to MTU=296 at qopen).  On every END byte the
     accumulated frame is putnext'd up; after I_LINK that delivers
     into IP's rput.

3. `slip_init()` (called from `main.c` after `ip_init`):
   - `miniuart_init(115200)`
   - `stream_build_kernel(&miniuart_streamtab, "slip0_serial", 0)`
   - `stream_push_kernel(sd, "slip")` -- now `head -> slip -> miniuart`
   - Spawns the `miniuart_rx` kthread; passes `sd->sd_drv_rq` so
     each batch of received bytes is `allocb`'d + `putnext`'d up
     into the SLIP module's rput
   - `netif_register(slip0)` with `streamtab=NULL` (pre-built)
   - `ip_attach_stream(sd, &slip0_nif)` -- I_LINK the slip stream
     under IP's control stream and backfill the netif identity

Default config: slip0 = 192.168.10.2/30, peer = 192.168.10.1,
MTU = 296.  Once SLIP is up, an outbound packet to anything in
`192.168.10.0/30` routes through slip0 automatically because IP's
longest-prefix-match (`ip_route` over `ip_lowers[]`) picks the
/30 over lo0's /8 for that subnet.

Host-side smoke test on Linux:

```
qemu-system-aarch64 -M raspi3b -kernel build/aarch64/kernel8.img \
    -serial mon:stdio -serial pty                       # second pty
# QEMU prints: char device redirected to /dev/pts/N
slattach -L -p slip -s 115200 /dev/pts/N &
ifconfig sl0 192.168.10.1 pointopoint 192.168.10.2 up
ping 192.168.10.2
```

`ip_attach_stream(struct stdata *, struct netif *)` is the new
in-`ipv4.c` helper for drivers that build their own bottom stream
(SLIP today; future Ethernet / virtio-net likewise).  Returns the
muxid and backfills the netif's row in `ip_lowers[]`.

UART rx is polled by the dedicated `miniuart_rx` kthread (same
shape PL011's `uart_rx_main` uses) -- no interrupt handler yet.
Switching all serial paths to interrupt-driven rx is a separate
cleanup.

Not yet:
- Inbound filter hook (would mirror outbound for the symmetric
  shape Solaris IPFilter has)
- TCP transport and `/dev/tcp`
- Interrupt-driven serial rx (replaces both PL011 and mini-UART
  kthreads with proper IRQ-driven byte delivery)
- ARP (only needed for Ethernet, not lo0 or SLIP)

## Signals

Per-thread state: `sig_pending` (bitmap), `sig_mask` (blocked bits),
and `sig_actions[NSIG]` (per-signal disposition, lazy-allocated on
first `sigaction()` call so threads that never install a handler pay
nothing).  POSIX numbering (SIGHUP=1, SIGINT=2, SIGKILL=9, SIGSEGV=11,
SIGTERM=15, …).

Delivery happens on the syscall-return path: `check_signals(tf)` runs
in `trap_dispatch`'s SVC handler after the impl returns and before
ERET.  It walks `pending & ~mask | pending & SIGBIT(SIGKILL)` — so
SIGKILL ignores the mask — picks the lowest bit, then takes one of:

- **`SIG_DFL`**: terminate (for `SIG_FATAL_MASK` signals) or drop.
- **`SIG_IGN`**: drop, clear pending bit.
- **user handler**: `sendsig(tf, sig, handler)` rewrites the trap
  frame so ERET vectors into the handler with `x0 = signo` and `x30`
  pointing at a kernel-emitted trampoline.

### sendsig / sigreturn frame surgery

`sendsig` carves a `struct sigframe` off the top of the user stack
(16-byte aligned), populated with:

- A two-instruction trampoline:
  ```
  MOVZ x8, #SYS_sigreturn
  SVC  #0
  ```
- The signal number (for debug).
- The saved `sig_mask`.
- The full saved trap frame (`x[0..30]`, `sp_el0`, `elr`, `spsr`).

The user region is mapped RWX (see `uts/aarch64/mmu.c`), so
executing the trampoline from stack is legal.  We do `DC CVAU` +
`IC IVAU` + `DSB` + `ISB` against the trampoline address after
writing, so real hardware sees the new instructions; QEMU TCG
doesn't strictly need it.

When the handler returns (`ret` → `x30` → trampoline), the trampoline
issues `SYS_sigreturn`.  That syscall is special-cased in `trap.c`
before generic dispatch because it has to mutate `tf` in place:
`sys_sigreturn_impl(tf)` copies the saved state out of the user
sigframe back into `tf`, restores `sig_mask`, and returns `tf->x[0]`
so the standard `tf->x[0] = dispatch(...)` assignment in trap.c is a
no-op.  `check_signals` is skipped on the way out — kicking another
delivery here would loop endlessly if the same signal is still
pending.

### Masking

`sa_mask` (caller-supplied) + `SIGBIT(sig)` (the signal itself) are
ORed into `sig_mask` before vectoring into the handler.  Sigreturn
restores `sig_mask` from the sigframe.  SIGKILL is never blockable
or catchable — enforced in both `sys_sigaction` (rejects the install)
and `check_signals` (OR-merges SIGKILL bits past the mask).

### sigprocmask / sigsuspend

`sys_sigprocmask(how, set, oldset)` does the obvious thing -- read
and modify `sig_mask` per SIG_BLOCK / SIG_UNBLOCK / SIG_SETMASK,
silently dropping SIGKILL out of any incoming mask.

`sys_sigsuspend(mask)` is the atomic "swap mask, wait for a signal,
restore mask, return -1" primitive POSIX needs for race-free signal
handling.  The mask-restore is *not* done in the syscall body --
that bug is what tanked the first masktest implementation, where
the mask got restored before `check_signals` had a chance to
deliver and the signal got re-blocked.  Instead, `sigsuspend`
stashes the pre-call mask in `sig_saved_mask` and sets
`sig_mask_save_pending`.  Then:

- If a handler is installed, `sendsig` populates the sigframe's
  `saved_mask` from `sig_saved_mask` (not the current `sig_mask`),
  clears the pending flag, and `sigreturn` later unwinds to the
  pre-call mask -- so the user sees `sigsuspend` return -1 with
  the original mask back in place.
- If no handler ran (signal was SIG_IGN'd, or the wake was
  incidental), `check_signals` restores `sig_mask` directly from
  `sig_saved_mask` at the end of its loop.

### `SA_RESETHAND` (one-shot delivery)

`sa_flags` has one bit defined so far: `SA_RESETHAND = 0x04`.  When
`sendsig` delivers via a handler whose disposition carries this
flag, the disposition is reset to `SIG_DFL` immediately after the
sigframe is built.  A second occurrence of the same signal takes
the default action.  The EL0 fault path uses this internally to
deliver SIGSEGV one-shot (see below); user code can also set it on
its own `sigaction()` if it wants the same semantics.

### EL0 SIGSEGV is catchable

A real EL0 synchronous fault (NULL deref, unmapped access, etc.)
no longer calls `sys_exit_impl` directly.  `trap_dispatch` checks
whether the thread installed a SIGSEGV handler and, if so, sets
`SA_RESETHAND` on the action, marks SIGSEGV pending, and falls
through into `check_signals(tf)`.  The handler runs exactly once
-- if it returns into the faulting instruction (sigreturn restores
the saved `tf->elr`), the re-fault hits a fresh `check_signals`
which sees `SIG_DFL` and kills the thread cleanly.  Without the
auto-reset we'd loop in the fault forever.

If no handler is installed, the behaviour is unchanged: `check_signals`
takes the default action (terminate) on the still-pending SIGSEGV.

### `sys_wait(tid)` and `thread_exit_wq`

Minimal "wait for a thread to exit" primitive.  A global
`struct wait_queue thread_exit_wq` lives in `uts/os/sched.c`.
`kthread_exit` sets `me->state = KT_DEAD` under `to_reap_lock` and
then calls `kthread_wake_all(&thread_exit_wq)` -- in that order, so
a waiter racing `kthread_find` after the wake observes
`state == KT_DEAD` (which `kthread_find` filters to `NULL`) and
returns 0 instead of sleeping again with nobody left to wake it.
`sys_wait_impl` loops: take `thread_exit_wq.sq_lock`, then
`kthread_find` → if gone, return 0; if a fatal signal landed,
return -1 (EINTR shape); otherwise
`kthread_sleep_on_locked(&thread_exit_wq, flags)` -- which atomically
links onto the queue and releases `sq_lock` after `context_switch`
commits sp.  `kthread_exit` sets `state=KT_DEAD` AND wakes any
waiters under the same `sq_lock`, so the check-then-sleep window is
closed: a waiter either sees the new state under its `sq_lock`
acquire or it sleeps and is woken under the exiter's `sq_lock`.

This isn't a real `waitpid` -- no parent-child tracking, no exit
status, no `WNOHANG` flag.  It's a `pthread_join`-shaped building
block to be used by the eventual proper `waitpid`.

### Ctrl-C → SIGINT

A minimal TTY line-discipline lives inside `uart_rx_main`
(`uts/os/stream_head.c`).  When the PL011 RX FIFO produces a 0x03
byte, the kernel doesn't push it upstream as data — it sends SIGINT
to the foreground reader of `/dev/console` instead.

Target selection has a two-tier fallback to ride out the typical
race where the shell is mid-loop processing the previous byte:

1.  The first thread on `sd->sd_readwait` if there is one (the
    blocked reader is unambiguously "foreground").
2.  Otherwise `sd->sd_last_reader` — the tid recorded by
    `stream_read` on entry, so the most recent reader is still
    findable even when the wait queue is momentarily empty.

`kthread_signal` does the work: it sets the pending bit and, if the
target is `KT_BLOCKED`, surgically extracts it from the wait queue
and marks it READY.  The blocked `stream_read` returns -1 (EINTR
shape) and `check_signals` on the syscall return delivers SIGINT to
the user handler if one is installed, or takes the default action
(terminate) otherwise.

`user/init.c` installs a SIGINT handler in `_start` that prints
`^C\r\n` and sets `sigint_pending`; `read_line` checks that flag at
the top of every loop iteration, clears its buffer, and re-prompts
— so the shell feels like bash on a partial input.

Current limitations (worth flagging):

- Only one foreground reader concept per stream; no SVR4 sessions or
  process groups yet.  Job control (`Ctrl-Z`, `bg`/`fg`) is not on
  the roadmap until pgrps land.
- vi / ked / kc share the SIGINT handler with the shell (they live
  in the same address space and same kthread).  Ctrl-C while editing
  will print "^C" over the editor's display.  Workaround: don't
  press Ctrl-C inside an editor; use the editor's own quit command.
  Proper fix is to save/restore SIGINT disposition around each
  editor invocation -- a small follow-up.

### EL0 faults

EL0 synchronous faults (page faults from user code) route through
`trap_dispatch`, which sets SIGSEGV pending on the offending thread
and calls `sys_exit_impl` directly — the kernel stays alive, the
thread dies.  A caught SIGSEGV handler installed via `sigaction`
would currently never run on a real EL0 fault; making that path
delivery-aware is straightforward (set pending, return into
`check_signals`) but raises livelock concerns (the faulting
instruction re-executes on handler return) so it's left for a
follow-up.

### Trap-exit IRQ masking (a sharp edge worth knowing)

The `KERNEL_EXIT` macro in `uts/aarch64/vectors.S` and the body of
`aarch64_enter_userspace` in `uts/aarch64/switch.S` both do the
same ERET-back-to-user dance:

```
msr daifset, #2        @ mask IRQs across the rest of the epilogue
msr elr_el1, ...       @ write the user PC
msr spsr_el1, ...      @ write the user PSTATE
... restore GPRs ...
eret
```

The `msr daifset, #2` at the top is load-bearing.  Without it, an
IRQ taken between the ELR_EL1 write and the final ERET lets the
hardware overwrite ELR_EL1 with the resume-here kernel PC.  When
the nested IRQ's own `KERNEL_EXIT` eret's back, the outer
epilogue's final ERET reads that stale ELR_EL1 and lands EL0 at a
kernel address.  The symptom is an `ec=0x20` instruction abort
with `ELR == FAR == trap_tail+0xc`, killing the user thread with
SIGSEGV after a stress run of edit-and-save loops.

The SVC handler intentionally runs preemptible (`msr daifclr, #2`
inside `trap_dispatch`), so the path back into the epilogue
arrives with IRQs unmasked — the explicit mask in the epilogue is
the only thing that closes the race.

## kallsyms + backtrace

A two-pass link.  Pass 1 produces an ELF; `tools/gen_kallsyms.sh`
nm's it and emits an .S file with a sorted (address, name_offset)
table plus a packed string blob.  Pass 2 links the populated table
into the `.kallsyms` section.

`.kallsyms` is placed after `.bss` in the linker script, so growing
it across passes doesn't shift any other section's symbols — the
addresses captured in pass 1 stay valid in pass 2.

`ksym_lookup(addr)` binary-searches; `kernel_backtrace_from(fp, lr)`
walks the AArch64 x29 frame chain.  `trap_dispatch` uses both on
unhandled faults so the panic dump tells you the function + offset.

## ftrace

`uts/os/ftrace.c` implements GCC `-finstrument-functions` hooks
(`__cyg_profile_func_{enter,exit}`) backed by a per-CPU 256-event
ring in BSS.  When the kernel is built with `make TRACE=1` every
non-excluded C function entry/exit becomes one event:

```
{ ts = CNTPCT_EL0,  fn,  caller,  cpu_id|kind }
```

`ftrace_init()` runs as the very first line of `kmain` so events
are captured from before `mmu_init` / `pmm_init`.  The hooks only
touch BSS (zeroed by `boot.S`) plus `CNTPCT_EL0` / `MPIDR_EL1`
system registers — no MMU, no pmm, no locks.  Each CPU writes only
its own ring (`cpu = MPIDR_EL1.Aff0`); no cross-CPU synchronisation.

A handful of TUs are compiled with `-fno-instrument-functions` to
prevent recursion or runaway noise: `ftrace.c`, `printk.c`,
`uart.c`, `string.c`, `kallsyms.c`.

`/proc/ftrace` is a STREAMS chrdev: read formats the ring with
`kallsyms`-resolved names, write parses `on` / `off` / `reset` ASCII
verbs.  See `docs/FTRACE.md`.

## SMP

All four Cortex-A53 cores run in EL1 with the MMU on, sharing the
single set of page tables built by core 0.  Each core has its own
`struct cpu` (the Solaris `cpu_t` shape) stored in the static
`cpus[4]` array in `uts/os/sched.c`; TPIDR_EL1 on each core holds
the address of its slot.  `curcpu()` is a single `mrs` instruction.

### Wake sequence

`smp_wake_secondary(cpu)` in `uts/os/main.c`:
1. `pmm_alloc`s a 4 KB kernel stack for the new core.
2. Stores the stack top in `smp_stacks[cpu]`.
3. Writes `secondary_start` into both our own `smp_release[cpu]`
   table and the QEMU/BCM2837 spin-table at `0xE0 + (cpu-1)*8`.
4. `dc cvac` flushes the cache lines so the secondary (MMU/cache
   off at this point) reads the freshest value from RAM.
5. `dsb sy; sev` wakes any WFE-blocked core.

### Secondary core boot (`uts/aarch64/boot.S` — `secondary_start`)

1. Read MPIDR to get `cpu_id` into x0.
2. Load stack top from `smp_stacks[cpu_id]` into x2.
3. Check CurrentEL: raspi3b QEMU drops secondaries at EL2.
4. If EL2: set `sp_el1 = x2`, program CNTHCTL_EL2, HCR_EL2.RW,
   SCTLR_EL1 (res1 bits, MMU off), SPSR_EL2 = EL1h+DAIF, ERET to
   `.Lsec_el1_entry`.
5. At `.Lsec_el1_entry`: x0 = cpu_id (GPRs not banked per EL in
   AArch64, so x0 survives the ERET), call `secondary_main(cpu_id)`.

### `secondary_main` (`uts/os/main.c`)

```
mmu_enable_this_cpu()   -- point TTBR0_EL1 at shared tables, enable M+C+I
trap_init()             -- msr vbar_el1 (same vector table as core 0)
sched_secondary_init()  -- struct cpu, idle kthread, set_curcpu()
msr daifclr, #2         -- unmask IRQs
for (;;) { kthread_yield(); wfi; }   -- idle loop
```

### Per-CPU scheduler state

`sched_secondary_init(cpu_id)` in `uts/os/sched.c`:
- Allocates a `kthread` for the idle thread (via `kmalloc`); this
  kthread represents the current execution context on this CPU.
- Sets `c->cpu_id`, `c->cpu_thread = idle`, `c->cpu_idle = idle`.
- Calls `set_curcpu(c)` to publish the struct via TPIDR_EL1.
- The idle thread's `stack_base` is NULL (the stack was allocated by
  `smp_wake_secondary` and lives forever).

### Dispatch queues, push-side balance, and idle steal

Each `struct cpu` carries an SVR4 `disp_t` -- one FIFO per priority
level guarded by `cpu_disp_lock`.  Priorities are `0..KSCHED_NPRI-1`
(64 levels); the active-priority bitmap `cpu_qactmap` is a single
`uint64_t` so picking the highest-priority runnable thread is one
`__builtin_clzll`.  `cpu_maxrunpri` caches the same value for
lock-free reads by the steal-side balancer, and `cpu_nrunnable` is
the total thread count across all priorities.

Priorities are driven by a scheduling-class layer (phase 4).  Two
classes ship today:

* **`SCLASS_SYS`** -- every kernel-only kthread (`uart_rx`, idle,
  `/proc/ps` readers, ...).  Fixed at `KSCHED_PRI_SYS_DEFAULT = 60`,
  no quantum tracking; `cl_tick` returns "no class-driven preempt"
  so SYS threads run until they voluntarily yield or block.

* **`SCLASS_TS`** -- every user-space (EL0) thread.  Driven by an
  inlined `ts_dptbl` row: each thread carries a `t_quantum_left`
  countdown reloaded to `TS_QUANTUM_TICKS = 5` (~50ms at HZ=100).
  `cl_tick` decrements it; on hit-zero the thread's priority is
  demoted by `TS_DEMOTE_STEP = 5` (floor 0).  `cl_wakeup` snaps
  priority back to `KSCHED_PRI_TS_DEFAULT = 30` and reloads the
  quantum -- I/O-bound threads stay at high priority, CPU-bound
  threads drift downward.

`sys_execve` / `sys_spawn` flip the new thread to `SCLASS_TS`
right after `kthread_create` and before the wake path picks it up,
so user processes pick up the aging behaviour automatically.
`kthread_setclass(t, cid)` is the public entry point.

Idle threads carry `KSCHED_PRI_IDLE = -1` as a documentation marker
-- they're never enqueued, they're the fallback that runs when every
priority is empty AND the cross-CPU steal also turns up nothing.

Both directions of load balancing are in play:

**Push-side** (`dispq_push` / `pick_push_target`).  When a thread
becomes runnable — `kthread_create` for a new thread,
`kthread_wake_all` / `kthread_signal` for a waker — we don't blindly
queue on the caller's CPU.  `pick_push_target` reads
`cpu_idle_mask` first; if any CPU is parked on its idle thread it
gets the work (and an IPI wakes it within microseconds).
Otherwise, if the caller's own queue already has ≥2 threads
waiting, we scan `cpus[]` for a strictly-shorter queue elsewhere
and push there.  Single-shot wakes onto an empty local queue stay
local for cache locality.

The remote `cpu_nrunnable` and `cpu_maxrunpri` reads are unlocked
— each is a single word, atomic on AArch64.  A stale value just
means a slightly worse pick; the subsequent locked push/pop lands
on whatever target we chose, which is correct either way.

**Pull-side** (`try_steal`, in `switch_to_next`).  When a CPU has
nothing in its own queue, it scans `cpus[]` for the peer with the
highest `cpu_maxrunpri` (so we steal the most important pending
work first, not just whatever's on the lowest-numbered CPU) and
pops one thread from its dispatch queue under that CPU's lock.
This is the catch-net for cases push-side missed (e.g. the waker
guessed wrong, or a thread is now blocking too long on another
CPU and a sibling went idle in the meantime).  If steal also
returns nothing:

- `requeue_current=1` (yield): keep running the current thread.
- `requeue_current=0` (block / exit): switch to `c->cpu_idle` so the
  core can WFI rather than spin.

Lock discipline: `switch_to_next` holds `cpu_disp_lock` for the
queue mutations and `cur` swap; IRQs stay masked throughout (the
lock is acquired with `spin_lock_irq_save` and only the spinlock is
released before `context_switch` — IRQs are restored from the saved
`flags` at the end of the function, after `context_switch` returns).
`dispq_push` acquires the *target* CPU's lock — possibly a remote
one — but it's a leaf lock (nothing else is held while it's held)
so cross-CPU deadlock isn't possible.

### Per-CPU timer

The ARMv8 generic timer is per-core: each `mrs CNTP_CTL_EL0` / `mrs
CNTP_TVAL_EL0` lands on the executing core's banked register.  The
BCM2836 ARM-local block routes each core's CNTPNSIRQ to that core's
IRQ line via `TIMER_CONTROL(N) = 0x40000040 + 4N`.  `irq_dispatch`
reads `IRQ_SOURCE(N) = 0x40000060 + 4N` to know who fired.

`timer_init_this_cpu()` (uts/aarch64/timer.c) writes the per-core
timer-control register, reloads CNTP_TVAL_EL0, and enables the
timer.  Core 0 calls it from `timer_init(hz)`; each secondary calls
it from `secondary_main` after MMU + traps are up.

Result: all four cores get scheduler ticks at the same rate.

### IPI (BCM2836 mailbox)

Inter-processor interrupts on BCM2836 use the mailbox MMIO at
`0x40000080..0xC0`.  Each core has four 32-bit write-set / read-clear
mailbox slots; writing to a peer's slot raises an IRQ on the peer if
the peer enabled that mailbox in `MBOX_IRQ_CTL(N) = 0x40000050+4N`.

`uts/aarch64/ipi.c`:
- `ipi_init_this_cpu()` enables mailbox 0 IRQ for the calling core.
- `ipi_send(cpu)` writes to a peer's mailbox 0 set + DSB SY.
- `ipi_handle()` clears the pending bit.  No payload is needed --
  the IRQ itself is the signal; returning from it drops back into
  the scheduler which retries `try_steal`.
- `ipi_wake_idle()` reads `sched_idle_mask()` and pokes every CPU
  currently parked on its idle thread.

The scheduler maintains `cpu_idle_mask`: a 4-bit bitmask, one bit
per core, set whenever a CPU switches TO its idle thread and cleared
when it leaves.  Updates happen under the per-CPU `cpu_disp_lock`.
Each `dispq_push` (from `kthread_create`, wake_one/wake_all,
`kthread_signal`, …) calls `ipi_wake_idle()` so idle cores notice
new work immediately instead of waiting for their next 10 ms tick.

### Idle thread is not a runqueue citizen

`c->cpu_idle` is a singleton owned by the CPU; it never goes on
`cpu_dispq`.  `switch_to_next` skips the requeue when the outgoing
thread IS the idle thread.  The empty-queue path returns to
`cpu_idle` directly whenever neither the local queue nor a remote
steal turn up real work.

### UART kprintf lock

`uts/os/printk.c` holds `kprintf_lock` (IRQ-save spinlock) for the
duration of one `kprintf` call.  Without it, every CPU writes to
`uart_putc` concurrently and bytes from different lines interleave.
`kpanic` deliberately bypasses the lock and writes via `uart_puts`
directly: another CPU may have crashed while holding the lock, and
the panic message must get out.

### What's still missing

| Gap                             | Notes                                       |
|---------------------------------|---------------------------------------------|
| Per-thread CPU affinity         | No affinity: push-side balancer picks the least-loaded CPU; idle steal redistributes |
| Pi 4 GICv2 backend              | The Pi 3 BCM2836 mailbox/timer-routing block is replaced by a real GIC on Pi 4 |
| Per-CPU `printk_buffered`       | The kprintf lock serialises but a CPU spinning waiting for the lock burns cycles -- per-CPU ring + a flusher would be lock-free |

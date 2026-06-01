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

`arch/aarch64/boot.S` is the reset vector.  All four raspi3b cores
enter at `_start`.  Core 0 (MPIDR_EL1.Aff0 == 0) proceeds; cores 1-3
park in `.Lpark` at WFI (see commit `aa8759f` for why not WFE).

Core 0 walks the EL ladder.  raspi3b drops us at EL2, so we set
SPSR_EL2 = EL1h, ELR_EL2 = continuation, eret.  Now at EL1 with all
DAIF masked, we zero `.bss`, set up an initial stack, and `bl kmain`.

`kmain` (in `kernel/main.c`) runs a fixed init sequence:

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
11. `ramdisk_init` + `kfs_mkimage` + `kfs_mount` — give us `/etc`.
12. Draw the splash on the framebuffer (one-time write; the kprintf
    tee that used to compete with QEMU's display thread is disabled
    by default — see commit `0cd91fd`).
13. `sched_init` (make `main` tid 0), `timer_init(100)`.
14. Spawn `uart_rx` and `user-init` kthreads.
15. Drop into the idle loop: `kthread_yield(); wfi;` forever.

## Memory

### Physical (pmm)

A freelist of 4 KB pages.  Each free page stores the next pointer in
its first 8 bytes.  Alloc pops the head, frees push.  O(1) both ways,
no metadata table.

`pmm_init(start, end)` is told two boundaries:
- `start` = `__kernel_end` (linker symbol, page-aligned past BSS)
- `end` = `min(PLAT_RAM_END, framebuffer_base)` — the GPU reserve trim

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

### MMU

Single TTBR0 covering 0..1 GiB.  L0/L1/L2 with the L2 split into 2 MB
blocks.  Most blocks are Normal cacheable inner-shareable.  The
peripheral window (0x3F000000..0x40000000 on Pi 3) is Device-nGnRE.

`mmu_map_user_2mb(va, pa)` overrides one L2 block with user-accessible
attributes (AP[2:1] = 01).  That's the userspace 2 MB at VA
0x10000000.

## Scheduling

`struct kthread` lives in `include/kappara/sched.h`.  Each has:

- `sp` — saved kernel SP (resumed by `context_switch`)
- `stack_base` — the page allocated for the kernel stack
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

`context_switch` (in `arch/aarch64/switch.S`) saves callee-saved regs
plus DAIF, swaps SP, restores callee-saved + DAIF from the new
thread.  Per-thread DAIF is critical (commit `0929814`): without it, a
thread that slept with IRQs masked would resume into whoever's DAIF
state the waker happened to have.

### Wait queues

`kthread_sleep_on(wq)` marks BLOCKED, threads onto `wq->head`,
yields without re-queuing.  `kthread_wake_all(wq)` walks the queue,
marks each READY, pushes to ready queue.

Used by `stream_read` for blocking I/O, by signal delivery to
surgically unlink a sleeper from its queue, and by pipe close to wake
the peer's reader with EOF.

### Reaping

`kthread_exit` parks the dying thread on `to_reap`.  The next
`switch_to_next` runs the reap loop **after** the context_switch — so
we're on a different stack, safe to `pmm_free` the dying thread's
stack and `kfree` its struct.

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

### Pipes

`sys_pipe` builds two pipe_end stdata and cross-wires
`a->sd_wq->q_next = b->sd_rq` (and vice versa).  Each end has
`sd_peer` pointing at the other.  Closing one end signals
`SD_EOF` on the peer and wakes its readers.

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

## kfs

Simple disk layout:

```
block 0   superblock (magic, num_files)
block 1   free-block bitmap
block 2   root dirent table
block 3+  data, subdir dirent tables
```

Each file gets `KFS_BLOCKS_PER_FILE` contiguous blocks pre-allocated.
The bitmap supports alloc/free, so `rm` actually returns blocks to
the pool (see commit `fb3f938`).

## Signals

Per-thread `sig_pending` bitmap.  POSIX numbers
(SIGHUP=1, SIGINT=2, SIGKILL=9, SIGSEGV=11, SIGTERM=15, ...).

Delivery happens on the syscall-return path (`check_signals` from
`trap_dispatch`).  A pending fatal signal causes `sys_exit_impl` on
the current thread.  No user-defined handlers yet — that needs
sendsig / sigreturn frame surgery, planned but not implemented.

EL0 synchronous faults (page faults from user code) route through
trap_dispatch into `check_signals` via setting SIGSEGV — the kernel
stays alive, only the faulting thread dies.

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

## SMP (foundation only)

Boot leaves cores 1-3 in QEMU's ARM spin-table at PA `0xE0`, `0xE8`,
`0xF0`.  Each polls its slot; writing the address of `secondary_start`
into the slot and issuing `DSB SY` + `SEV` releases that core.

`smp_wake_secondary(cpu)` in `kernel/main.c`:
1. `pmm_alloc`s a 4 KB kernel stack for the new CPU.
2. Stores the stack top in `smp_stacks[cpu]`.
3. Writes the entry into both our own `smp_release[cpu]` table (used
   if a future PSCI boot path ever has the cores park in our
   `.Lpark` first) and into the QEMU spin-table at
   `0xE0 + (cpu-1)*8`.
4. `dc cvac` on each slot so the secondary (MMU + cache off) reads
   the freshest value from RAM.
5. `dsb sy; sev` wakes anyone WFE-blocked.

The asm entry `secondary_start` (boot.S) reads its own MPIDR for
the CPU id, picks up its stack from `smp_stacks[cpu]`, and `bl`s
`secondary_main` in C.  The C side prints `smp: cpu N` and drops
into `for (;;) wfi`.

This is the foundation — the cores are alive, executing C code, and
asleep on WFI.  What's still missing for a real SMP kernel:

| Step                                     | Why we don't have it yet                  |
|------------------------------------------|-------------------------------------------|
| Per-CPU `cur` via TPIDR_EL1              | Today `cur` is a single global            |
| Spinlock on ready queue                  | Single-CPU never needed it                |
| Each CPU in the scheduler loop           | Secondaries idle, don't pull threads      |
| Per-CPU generic timer setup              | Only core 0 has CNTPNSIRQ routed          |
| IPI for cross-CPU wake                   | No ready-queue wake needed yet            |
| Cache-coherent locking primitives        | No shared mutable state yet               |
| MMU + EL1 drop for secondaries           | They stay at EL2 with MMU off             |

That list is the next session.  At boot you'll see four lines (one
per CPU including core 0) interleaved at the byte level on the UART
because every core writes through the same `uart_putc` with no lock
— harmless, expected, will be the first thing a `kprintf_lock`
fixes.

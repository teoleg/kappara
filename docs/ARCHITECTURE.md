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

The user region is mapped RWX (see `arch/aarch64/mmu.c`), so
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

`kernel/ftrace.c` implements GCC `-finstrument-functions` hooks
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
`cpus[4]` array in `kernel/sched.c`; TPIDR_EL1 on each core holds
the address of its slot.  `curcpu()` is a single `mrs` instruction.

### Wake sequence

`smp_wake_secondary(cpu)` in `kernel/main.c`:
1. `pmm_alloc`s a 4 KB kernel stack for the new core.
2. Stores the stack top in `smp_stacks[cpu]`.
3. Writes `secondary_start` into both our own `smp_release[cpu]`
   table and the QEMU/BCM2837 spin-table at `0xE0 + (cpu-1)*8`.
4. `dc cvac` flushes the cache lines so the secondary (MMU/cache
   off at this point) reads the freshest value from RAM.
5. `dsb sy; sev` wakes any WFE-blocked core.

### Secondary core boot (`arch/aarch64/boot.S` — `secondary_start`)

1. Read MPIDR to get `cpu_id` into x0.
2. Load stack top from `smp_stacks[cpu_id]` into x2.
3. Check CurrentEL: raspi3b QEMU drops secondaries at EL2.
4. If EL2: set `sp_el1 = x2`, program CNTHCTL_EL2, HCR_EL2.RW,
   SCTLR_EL1 (res1 bits, MMU off), SPSR_EL2 = EL1h+DAIF, ERET to
   `.Lsec_el1_entry`.
5. At `.Lsec_el1_entry`: x0 = cpu_id (GPRs not banked per EL in
   AArch64, so x0 survives the ERET), call `secondary_main(cpu_id)`.

### `secondary_main` (`kernel/main.c`)

```
mmu_enable_this_cpu()   -- point TTBR0_EL1 at shared tables, enable M+C+I
trap_init()             -- msr vbar_el1 (same vector table as core 0)
sched_secondary_init()  -- struct cpu, idle kthread, set_curcpu()
msr daifclr, #2         -- unmask IRQs
for (;;) { kthread_yield(); wfi; }   -- idle loop
```

### Per-CPU scheduler state

`sched_secondary_init(cpu_id)` in `kernel/sched.c`:
- Allocates a `kthread` for the idle thread (via `kmalloc`); this
  kthread represents the current execution context on this CPU.
- Sets `c->cpu_id`, `c->cpu_thread = idle`, `c->cpu_idle = idle`.
- Calls `set_curcpu(c)` to publish the struct via TPIDR_EL1.
- The idle thread's `stack_base` is NULL (the stack was allocated by
  `smp_wake_secondary` and lives forever).

### Dispatch queues and idle steal

Each `struct cpu` has a FIFO `cpu_dispq` guarded by `cpu_disp_lock`.
`kthread_create` pushes the new thread onto the creating CPU's queue.
`switch_to_next` pops the local queue first; if empty it calls
`try_steal`, which scans `cpus[]` and pops one thread from the first
non-empty remote queue (under that CPU's lock).  If steal also returns
nothing:

- `requeue_current=1` (yield): keep running the current thread.
- `requeue_current=0` (block / exit): switch to `c->cpu_idle` so the
  core can WFI rather than spin.

Lock discipline: `switch_to_next` holds `cpu_disp_lock` for the
queue mutations and `cur` swap; IRQs stay masked throughout (the
lock is acquired with `spin_lock_irq_save` and only the spinlock is
released before `context_switch` — IRQs are restored from the saved
`flags` at the end of the function, after `context_switch` returns).

### Per-CPU timer

The ARMv8 generic timer is per-core: each `mrs CNTP_CTL_EL0` / `mrs
CNTP_TVAL_EL0` lands on the executing core's banked register.  The
BCM2836 ARM-local block routes each core's CNTPNSIRQ to that core's
IRQ line via `TIMER_CONTROL(N) = 0x40000040 + 4N`.  `irq_dispatch`
reads `IRQ_SOURCE(N) = 0x40000060 + 4N` to know who fired.

`timer_init_this_cpu()` (arch/aarch64/timer.c) writes the per-core
timer-control register, reloads CNTP_TVAL_EL0, and enables the
timer.  Core 0 calls it from `timer_init(hz)`; each secondary calls
it from `secondary_main` after MMU + traps are up.

Result: all four cores get scheduler ticks at the same rate.

### IPI (BCM2836 mailbox)

Inter-processor interrupts on BCM2836 use the mailbox MMIO at
`0x40000080..0xC0`.  Each core has four 32-bit write-set / read-clear
mailbox slots; writing to a peer's slot raises an IRQ on the peer if
the peer enabled that mailbox in `MBOX_IRQ_CTL(N) = 0x40000050+4N`.

`arch/aarch64/ipi.c`:
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

`kernel/printk.c` holds `kprintf_lock` (IRQ-save spinlock) for the
duration of one `kprintf` call.  Without it, every CPU writes to
`uart_putc` concurrently and bytes from different lines interleave.
`kpanic` deliberately bypasses the lock and writes via `uart_puts`
directly: another CPU may have crashed while holding the lock, and
the panic message must get out.

### What's still missing

| Gap                             | Notes                                       |
|---------------------------------|---------------------------------------------|
| Per-thread CPU affinity         | Pushes onto the waker's CPU and lets idle steal redistribute |
| Push-side load balancing        | Currently only pull-side (idle steal); a busy CPU's queue gets long-ish before another core grabs from it |
| Pi 4 GICv2 backend              | The Pi 3 BCM2836 mailbox/timer-routing block is replaced by a real GIC on Pi 4 |
| Per-CPU `printk_buffered`       | The kprintf lock serialises but a CPU spinning waiting for the lock burns cycles -- per-CPU ring + a flusher would be lock-free |

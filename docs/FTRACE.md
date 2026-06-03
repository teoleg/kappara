# ftrace — per-CPU function tracer

A bring-up debug aid for inspecting what the kernel actually does, in
order, during the parts of boot that are too early or too fast for
`kprintf` to be useful.

## TL;DR

```
make ARCH=aarch64 TRACE=1
make run
... boot ...
kappara:/# ftrace off     # freeze the ring -- nothing new gets recorded
kappara:/# ftrace dump    # dump the last 256 events per CPU
```

When the kernel is built without `TRACE=1` (the default) the tracer is
inert: the cyg-profile hooks are never emitted, `/proc/ftrace` exists
but contains nothing. There is no runtime cost in production builds.

## How it works

`-finstrument-functions` (a GCC pass) inserts two function calls at
every C function's prologue and epilogue:

```
void some_function(args) {
    __cyg_profile_func_enter(&some_function, __builtin_return_address(0));
    /* body */
    __cyg_profile_func_exit (&some_function, __builtin_return_address(0));
}
```

`uts/os/ftrace.c` implements those two hooks. Each call writes one
event into a per-CPU ring buffer:

```c
struct ftrace_event {
    uint64_t ts;        // CNTPCT_EL0 timestamp
    uint64_t fn;        // function whose enter/exit this is
    uint64_t caller;    // caller PC
    uint32_t cpu_kind;  // (cpu_id << 8) | (0=enter | 1=exit)
};
```

Per CPU:
- A power-of-two ring of `FT_RING_SZ = 256` events lives in **BSS**.
- A monotonic `heads[cpu]` counter says how many events have ever been
  recorded; the ring index is `heads[cpu] & (FT_RING_SZ-1)`.
- `total - FT_RING_SZ` is the count of events that have been
  overwritten (dropped).
- `cpu_id` comes from `MPIDR_EL1.Aff0` (one `mrs`), independent of
  whether the scheduler has initialised TPIDR_EL1 yet.
- `ts` comes from `CNTPCT_EL0` (one `mrs`); available from reset.

No locks: each CPU only writes its own ring. The dump path sets a
per-CPU `in_dump[]` flag so the act of formatting + `allocb` +
`putnext` (themselves instrumented) doesn't pollute the trace it's
about to print.

## Why it works pre-MMU / pre-pmm

`ftrace_init()` is called as the very first line of `kmain`, before
`mmu_init` or `pmm_init`. The hooks only touch:

- BSS storage (zeroed by `boot.S`)
- `CNTPCT_EL0` and `MPIDR_EL1` (system registers; no MMU, no caches)

So instrumentation captures the very first C function entries after
reset — including `mmu_init`, `pmm_init`, `kmem_init`. The ring
keeps the **most recent** 256 events per CPU, so if you want to see
those very-early entries you must freeze the trace before they roll
out (see "Workflow", below).

## Files that opt out

The Makefile compiles a short list of TUs with
`-fno-instrument-functions` regardless of `TRACE=1`. They sit on the
tracer's own path or are called so often that instrumenting them
would multiply event volume for no diagnostic value:

| File                | Why                                        |
|---------------------|--------------------------------------------|
| `uts/os/ftrace.c`   | the tracer itself (would recurse on its own hooks) |
| `uts/os/printk.c`   | the dump's `kprintf` path |
| `uts/aarch64/uart.c` | `uart_putc` called from `printk` |
| `uts/os/string.c`   | `kmemset`/`kmemcpy` called from every code path |
| `uts/os/kallsyms.c` | the dump's `ksym_lookup` resolves names |

`.S` files are never instrumented (the flag is a C-only pass).

## Controlling the tracer

The runtime API is small (`include/kappara/ftrace.h`):

| Call                  | Effect                                          |
|-----------------------|-------------------------------------------------|
| `ftrace_init()`       | Zero rings, enable (idempotent)                 |
| `ftrace_enable()`     | Re-arm recording; preserves ring contents       |
| `ftrace_disable()`    | Freeze recording; ring is preserved             |
| `ftrace_reset()`      | Drop all events; counters back to zero          |
| `ftrace_dump_to_q(q,N)` | Stream last N events per CPU into a STREAMS queue |

From userspace, `/proc/ftrace` exposes the same surface:

- `read` (e.g. `cat /proc/ftrace` or `ftrace dump`) — formats the
  ring as `[ts] e|x fn+off <- caller+off` lines.
- `write` (e.g. `ftrace off` or `echo on > /proc/ftrace`) — parses
  `on` / `off` / `reset`.

## Workflow: capture early-boot events

The 256-event ring rolls over fast — the shell idle loop produces
thousands of events per second. To actually look at early boot you
need to **freeze the trace** before the noise rolls it out:

1. Edit the bring-up code: add `ftrace_disable()` (or `ftrace_reset()`
   followed by `ftrace_disable()`) at the exact point you want a
   snapshot. Example: right after `pmm_init` returns.
2. `make ARCH=aarch64 TRACE=1 && make run`
3. Wait for the shell prompt.
4. `ftrace dump` — shows the 256 events leading up to your
   `ftrace_disable()` call.

For ad-hoc captures of later-stage activity:

1. `ftrace reset` immediately before doing the thing of interest.
2. Do the thing.
3. `ftrace off` to freeze.
4. `ftrace dump`.

## Output format

```
ftrace: enabled  ring_per_cpu=256

[cpu 0] events=538004 (dropped=537748) shown=256
[250535828] e stream_write  <- sys_write_impl+0x7c
[250535844] e stream_putmsg  <- stream_write+0x58
[250535874] e allocb  <- stream_putmsg+0x124
[250535892] e kmem_cache_alloc  <- allocb+0x4c
[250535920] x kmem_cache_alloc  <- allocb+0x4c
...
```

- `[ts]` — `CNTPCT_EL0` value at the hook. Subtracting two
  consecutive timestamps gives a tick count; on the Pi 3 generic
  timer (62.5 MHz nominal) one tick is 16 ns.
- `e`/`x` — function entry vs exit.
- `fn` — instrumented function. Symbol via `kallsyms`; falls back to
  raw `0xADDR` if the symbol table can't resolve it.
- `caller` — `__builtin_return_address(0)` at the hook site. Shown
  as `name+0xoffset` so you can see exactly which call site in the
  caller produced this trace.

## Limits and pitfalls

- **256 events per CPU is small.** Suited to "what just happened?"
  not "what was happening five seconds ago?". Bump `FT_RING_SZ` in
  `uts/os/ftrace.c` if you need more (BSS cost: `4 * FT_RING_SZ * 32`
  bytes).
- **Per-CPU only.** Cross-CPU correlation requires reading several
  rings and sorting by `ts` (CNTPCT_EL0 is system-wide on the Pi 3,
  so timestamps are comparable between cores).
- **`static inline` functions are de-inlined by `-finstrument-functions`**
  so the hook can fire. Expect `spin_lock`/`spin_unlock` etc. to
  show up as real calls in the trace even though they normally
  inline. This is also why we exclude `string.c` — `kmemset` would
  otherwise dominate every trace.
- **No filtering yet.** Every C function entry/exit records. A
  per-function on/off mask, function-name filter, or
  `--exclude=foo,bar` build flag is a follow-up.
- **Interrupt context is fine.** The hook is reentrancy-safe: it
  uses only per-CPU state and the `enabled` / `in_dump[]` checks
  short-circuit if recording is paused.

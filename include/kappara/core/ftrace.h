/*
 * include/kappara/ftrace.h -- per-CPU function tracer
 *
 * What this is
 * ------------
 * A bring-up debug aid: when the kernel is built with TRACE=1
 * (make TRACE=1) gcc's -finstrument-functions inserts a call to
 *   __cyg_profile_func_enter (fn, caller)
 *   __cyg_profile_func_exit  (fn, caller)
 * at every function's entry / return.  ftrace.c implements those
 * two hooks: they write one event into a per-CPU ring buffer.
 *
 * Why "earlier stages"
 * --------------------
 * The ring lives in BSS (zeroed by boot.S) and the hooks only touch
 * MMIO/sysregs (CNTPCT_EL0 for timestamps, MPIDR_EL1.Aff0 for cpu_id)
 * plus that BSS array.  No kmalloc, no pmm, no locks, no MMU magic.
 * `ftrace_init()` can therefore be called as the very first thing in
 * kmain, before mmu_init or pmm_init, and capture every function
 * entry from there onward.
 *
 * Layout
 * ------
 * Per cpu, a power-of-two ring of fixed-size events:
 *
 *   rings[cpu][heads[cpu] & MASK] = { ts, fn, caller, cpu|kind }
 *
 * heads[cpu] is monotonic; total events written = heads[cpu], dropped
 * (overwritten) = max(0, heads[cpu] - RING_SZ).  No locks: each CPU
 * writes only its own ring, and re-entry into the hook from within
 * the dumper is suppressed by per-CPU in_dump[].
 *
 * Reading
 * -------
 *   cat /proc/ftrace      newest LAST_N events per cpu, formatted as
 *                         "[ts]  e fn+off  <-- caller+off"
 *
 * When TRACE=0 (the default), the cyg hooks are NEVER called -- no
 * instrumentation means no events.  /proc/ftrace still exists; it
 * just dumps an empty trace.  This file's API is link-safe in either
 * build.
 */
#ifndef KAPPARA_FTRACE_H
#define KAPPARA_FTRACE_H

#include <stdint.h>

struct queue;	/* fwd; STREAMS queue_t -- see kappara/streams.h */

/* Zero the rings and (re)arm the tracer.  Idempotent; safe to call
 * before pmm/mmu/sched are up. */
void  ftrace_init(void);

/* Runtime toggles.  Disable suppresses recording globally; the ring
 * is preserved so a subsequent dump still shows the captured events. */
void  ftrace_enable(void);
void  ftrace_disable(void);
int   ftrace_is_enabled(void);

/* Drop all events; head/total counters return to zero. */
void  ftrace_reset(void);

/* Dump newest-first up to `last_n_per_cpu` events from each CPU into
 * `q` as a sequence of mblks, then putnext an M_HANGUP so a reader
 * sees EOF after draining.  Sets the per-CPU in_dump guard around the
 * dump so the act of formatting + allocb + putnext does not pollute
 * the trace it is dumping. */
void  ftrace_dump_to_q(struct queue *q, unsigned last_n_per_cpu);

#endif

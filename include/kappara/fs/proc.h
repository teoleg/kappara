/*
 * include/kappara/proc.h -- procfs (Linux-flavored introspection over STREAMS)
 *
 * /proc on kappara is not its own filesystem -- it's a directory of
 * STREAMS chrdevs, each one a tiny read-only driver whose
 * qi_qopen primes its read queue with a freshly-formatted text
 * snapshot of some kernel state.  Reading the device drains that
 * snapshot; closing it discards what was unread.  The same pattern
 * /dev/klog has used since the beginning.
 *
 * Current files
 * -------------
 *   /proc/ps         every thread: tid, state, name
 *   /proc/meminfo    pmm + total slab bytes (vmstat-ish summary)
 *   /proc/slabinfo   one line per size-cache: name, obj_size,
 *                    free / total objects
 *   /proc/streams    one line per registered STREAMS module/driver
 */

#ifndef KAPPARA_PROC_H
#define KAPPARA_PROC_H

void proc_init(void);

#endif

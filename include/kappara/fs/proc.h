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
 *   /proc/cpuload    per-CPU load + idle ratio (smp-aware)
 *   /proc/ftrace     ftrace ring dump (read) + on/off/reset (write)
 *   /proc/netif      registered interfaces + IP/netmask
 *   /proc/slip       slip0 byte / frame counters
 *   /proc/tcp        TCB table: state, ports, rtt, cwnd, ...
 *   /proc/acpi       AWS.md stage A-C: ACPI table summary
 *                    (RSDP, GIC base, CPU MPIDRs, ECAM, timer GSIVs)
 *   /proc/pci        AWS.md stage D: enumerated PCIe devices
 *                    (bdf, vid:did, class, hdr, MSI-X cap, BARs)
 *   /proc/efi        AWS.md stage B: EFI memory map
 *                    (type, start, pages, end)
 */

#ifndef KAPPARA_PROC_H
#define KAPPARA_PROC_H

void proc_init(void);

#endif

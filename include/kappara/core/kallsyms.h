/*
 * include/kappara/kallsyms.h -- kernel symbol table + backtrace
 *
 * After link, a post-build pass runs `nm` on the kernel ELF and
 * emits a sorted table of every text symbol's (address, name).
 * The table is appended into the .kallsyms section, which the
 * linker script places after BSS so its size doesn't shift any
 * code or data addresses -- a one-pass scheme that gets us
 * function names from a raw address.
 *
 * Layout in the image (see tools/gen_kallsyms.sh):
 *
 *     __kallsyms_magic    .4byte 'KSYM' little-endian (0x4d59534b)
 *     __kallsyms_count    .4byte N
 *     __kallsyms_table    N * struct { u64 addr; u32 strofs; u32 pad; }
 *     __kallsyms_strings  packed NUL-terminated names
 *
 * Use
 * ---
 *   const char *fn = ksym_lookup(addr, &offset);
 *   kprintf("at %s+0x%lx\n", fn ? fn : "?", offset);
 *
 *   kernel_backtrace();   -- walks fp/lr chain via x29/x30, prints
 *                            one line per frame.  Stops at NULL fp
 *                            or 16 frames, whichever comes first.
 */
#ifndef KAPPARA_KALLSYMS_H
#define KAPPARA_KALLSYMS_H

#include <stdint.h>

const char *ksym_lookup(uint64_t addr, uint64_t *offset_out);
void        kernel_backtrace(void);

/* Walk starting from a saved (fp, lr) pair rather than the caller's
 * own registers.  Used from the trap path so the printed frames
 * cover the faulting context instead of the trap handler. */
void        kernel_backtrace_from(uint64_t fp, uint64_t lr);

/* Walk every (addr, name) pair in registration order.  Backs
 * /dev/ksyms.  `cb` returns non-zero to stop iteration;
 * ksym_for_each returns that value (or 0 when it ran to the end).
 * Returns 0 immediately when the symbol table isn't populated
 * (e.g. between linker passes, or on a stripped build). */
int         ksym_for_each(int (*cb)(uint64_t addr, const char *name,
				    void *arg),
			  void *arg);

#endif

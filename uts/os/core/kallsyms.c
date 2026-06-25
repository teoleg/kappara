/*
 * kernel/kallsyms.c -- symbol lookup + frame-pointer backtrace
 *
 * The symbol table lives in the .kallsyms section, populated by
 * tools/gen_kallsyms.sh after the first link.  Format is documented
 * in include/kappara/core/kallsyms.h; entries are sorted by address so
 * lookup is a binary search.
 *
 * Backtrace
 * ---------
 * AArch64 ABI: each function (when not omit-frame-pointer'd) keeps
 * x29 pointing at a 16-byte saved frame [next_fp, saved_lr].  Walk
 * that chain to recover the call stack.  We compile with -O2 which
 * may inline / tail-call past frames; the printed backtrace is
 * therefore best-effort, not exhaustive, but in practice covers
 * everything useful for a hobby kernel's debug needs.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/core/kallsyms.h"
#include "kappara/core/printk.h"

#define KSYM_MAGIC	0x4d59534bu	/* 'KSYM' little-endian */

struct ksym_entry {
	uint64_t	addr;
	uint32_t	str_off;
	uint32_t	pad;
};

extern uint32_t            __kallsyms_magic;
extern uint32_t            __kallsyms_count;
extern struct ksym_entry   __kallsyms_table[];
extern char                __kallsyms_strings[];

static int kallsyms_ok(void)
{
	return __kallsyms_magic == KSYM_MAGIC && __kallsyms_count > 0;
}

const char *ksym_lookup(uint64_t addr, uint64_t *offset_out)
{
	if (!kallsyms_ok()) return NULL;

	/* Find largest entry with addr <= target. */
	uint32_t lo = 0, hi = __kallsyms_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (__kallsyms_table[mid].addr <= addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0) return NULL;
	lo--;

	if (offset_out) *offset_out = addr - __kallsyms_table[lo].addr;
	return &__kallsyms_strings[__kallsyms_table[lo].str_off];
}

void kernel_backtrace_from(uint64_t fp, uint64_t lr)
{
	if (!kallsyms_ok()) {
		kprintf("backtrace: symbol table not embedded\n");
		return;
	}

	kprintf("backtrace:\n");
	for (int depth = 0; depth < 16; depth++) {
		uint64_t off = 0;
		const char *name = lr ? ksym_lookup(lr, &off) : NULL;
		kprintf("  #%d  0x%016lx  %s+0x%lx\n",
			depth, (unsigned long)lr,
			name ? name : "?",
			(unsigned long)off);

		if (!fp) break;
		/* Sanity: fp must look like a kernel address.  We don't
		 * know exact stack bounds here (per-thread); reject anything
		 * obviously bogus (below 1 MiB or above the bottom 1 GiB
		 * since pmm caps there). */
		if (fp < 0x100000UL || fp >= 0x40000000UL) break;
		uint64_t *frame = (uint64_t *)(uintptr_t)fp;
		uint64_t next_fp = frame[0];
		uint64_t next_lr = frame[1];
		if (next_fp && next_fp <= fp) break;	/* cycle guard */
		fp = next_fp;
		lr = next_lr;
	}
}

void kernel_backtrace(void)
{
	uint64_t fp, lr;
	__asm__ volatile ("mov %0, x29" : "=r"(fp));
	__asm__ volatile ("mov %0, x30" : "=r"(lr));
	kernel_backtrace_from(fp, lr);
}

/* Public iterator over the symbol table.  Calls `cb` once per
 * (addr, name) pair in the order they appear in __kallsyms_table
 * (sorted by address, the gen_kallsyms.sh post-pass guarantees
 * that).  Backs /dev/ksyms. */
int ksym_for_each(int (*cb)(uint64_t addr, const char *name, void *arg),
		  void *arg)
{
	if (!kallsyms_ok()) return 0;
	for (uint32_t i = 0; i < __kallsyms_count; i++) {
		const char *name = &__kallsyms_strings[__kallsyms_table[i].str_off];
		int r = cb(__kallsyms_table[i].addr, name, arg);
		if (r) return r;
	}
	return 0;
}

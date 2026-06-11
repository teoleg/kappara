/*
 * kernel/ftrace.c -- per-CPU function tracer (cyg-profile based)
 * ==============================================================
 *
 * See include/kappara/core/ftrace.h for the contract.  This TU is always
 * compiled with -fno-instrument-functions (overridden in the Makefile)
 * so the tracer's own code does not recurse into itself.  Likewise
 * for printk.c, uart.c, string.c, and kallsyms.c -- they sit on the
 * dump path and would otherwise paint themselves into every trace.
 *
 * Why per-cpu ring with no lock
 * -----------------------------
 * Each CPU writes only its own ring (cpu = MPIDR_EL1.Aff0).  Hooks
 * fire from the same execution context the function call did, so
 * "which CPU is recording" never changes mid-hook.  The only re-entry
 * worry is the dumper allocating mblks + putnext'ing -- those calls
 * are themselves instrumented when TRACE=1, so we set `in_dump[cpu]`
 * around the dump body and the hook fast-paths back out.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/core/ftrace.h"
#include "kappara/core/kallsyms.h"
#include "kappara/io/streams.h"

#define FT_NCPU		4
#define FT_RING_SZ	256		/* must be power of 2 */
#define FT_MASK		(FT_RING_SZ - 1)

#define FT_KIND_ENTER	0
#define FT_KIND_EXIT	1

struct ftrace_event {
	uint64_t ts;
	uint64_t fn;
	uint64_t caller;
	uint32_t cpu_kind;	/* (cpu_id << 8) | kind */
	uint32_t pad;
};

static struct ftrace_event rings[FT_NCPU][FT_RING_SZ];
static uint32_t            heads[FT_NCPU];	/* monotonic: total events ever */
static volatile int        in_dump[FT_NCPU];	/* re-entry guard */
static volatile int        enabled;

static inline uint64_t ft_ts(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static inline unsigned ft_cpu(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, mpidr_el1" : "=r"(v));
	return (unsigned)(v & 0xff);
}

static inline void ft_record(uint64_t fn, uint64_t caller, unsigned kind)
{
	if (!enabled) return;
	unsigned cpu = ft_cpu();
	if (cpu >= FT_NCPU) return;
	if (in_dump[cpu]) return;
	uint32_t h = heads[cpu];
	heads[cpu] = h + 1;
	struct ftrace_event *e = &rings[cpu][h & FT_MASK];
	e->ts       = ft_ts();
	e->fn       = fn;
	e->caller   = caller;
	e->cpu_kind = (cpu << 8) | kind;
}

/* ---- GCC -finstrument-functions hooks ------------------------------ */

void __cyg_profile_func_enter(void *fn, void *caller);
void __cyg_profile_func_exit (void *fn, void *caller);

void __cyg_profile_func_enter(void *fn, void *caller)
{
	ft_record((uint64_t)(uintptr_t)fn,
		  (uint64_t)(uintptr_t)caller,
		  FT_KIND_ENTER);
}

void __cyg_profile_func_exit(void *fn, void *caller)
{
	ft_record((uint64_t)(uintptr_t)fn,
		  (uint64_t)(uintptr_t)caller,
		  FT_KIND_EXIT);
}

/* ---- Public control ------------------------------------------------- */

void ftrace_init(void)
{
	for (unsigned c = 0; c < FT_NCPU; c++) {
		heads[c]   = 0;
		in_dump[c] = 0;
		for (unsigned i = 0; i < FT_RING_SZ; i++) {
			rings[c][i].ts       = 0;
			rings[c][i].fn       = 0;
			rings[c][i].caller   = 0;
			rings[c][i].cpu_kind = 0;
		}
	}
	enabled = 1;
}

void ftrace_enable(void)  { enabled = 1; }
void ftrace_disable(void) { enabled = 0; }
int  ftrace_is_enabled(void) { return enabled; }

void ftrace_reset(void)
{
	int was = enabled;
	enabled = 0;
	for (unsigned c = 0; c < FT_NCPU; c++)
		heads[c] = 0;
	enabled = was;
}

/* ---- Dump path ------------------------------------------------------ */

struct ftbuf {
	char     data[1024];
	unsigned len;
	queue_t *q;
};

static void ftb_flush(struct ftbuf *b)
{
	if (b->len == 0) return;
	mblk_t *mp = allocb(b->len, 0);
	if (mp) {
		unsigned char *dst = mp->b_wptr;
		for (unsigned i = 0; i < b->len; i++)
			dst[i] = (unsigned char)b->data[i];
		mp->b_wptr += b->len;
		putnext(b->q, mp);
	}
	b->len = 0;
}

static void ftb_putc(struct ftbuf *b, char c)
{
	if (b->len >= sizeof(b->data))
		ftb_flush(b);
	b->data[b->len++] = c;
}

static void ftb_str(struct ftbuf *b, const char *s)
{
	while (*s) ftb_putc(b, *s++);
}

static void ftb_u(struct ftbuf *b, unsigned long v, int min_width)
{
	char tmp[32];
	int  i = 0;
	if (v == 0) tmp[i++] = '0';
	while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
	while (i < min_width) { ftb_putc(b, ' '); min_width--; }
	while (i--) ftb_putc(b, tmp[i]);
}

static void ftb_hex(struct ftbuf *b, unsigned long v)
{
	static const char digs[] = "0123456789abcdef";
	ftb_putc(b, '0'); ftb_putc(b, 'x');
	int started = 0;
	for (int shift = 60; shift >= 0; shift -= 4) {
		unsigned nib = (v >> shift) & 0xfu;
		if (nib || started || shift == 0) {
			ftb_putc(b, digs[nib]);
			started = 1;
		}
	}
}

static void ftb_addr(struct ftbuf *b, uint64_t addr)
{
	uint64_t off = 0;
	const char *name = ksym_lookup(addr, &off);
	if (name) {
		ftb_str(b, name);
		if (off) { ftb_putc(b, '+'); ftb_hex(b, off); }
	} else {
		ftb_hex(b, addr);
	}
}

static void dump_cpu(struct ftbuf *b, unsigned cpu, unsigned want)
{
	uint32_t total = heads[cpu];
	if (total == 0) return;
	uint32_t shown = total > FT_RING_SZ ? FT_RING_SZ : total;
	if (want && want < shown) shown = want;

	uint32_t start = total - shown;	/* index of oldest of the `shown` */

	ftb_str(b, "\n[cpu ");
	ftb_u(b, cpu, 0);
	ftb_str(b, "] events="); ftb_u(b, total, 0);
	if (total > FT_RING_SZ) {
		ftb_str(b, " (dropped="); ftb_u(b, total - FT_RING_SZ, 0);
		ftb_putc(b, ')');
	}
	ftb_str(b, " shown="); ftb_u(b, shown, 0);
	ftb_putc(b, '\n');

	for (uint32_t i = 0; i < shown; i++) {
		struct ftrace_event *e = &rings[cpu][(start + i) & FT_MASK];
		unsigned kind = e->cpu_kind & 0xff;
		ftb_putc(b, '[');
		ftb_u(b, (unsigned long)e->ts, 0);
		ftb_str(b, "] ");
		ftb_putc(b, kind == FT_KIND_ENTER ? 'e' : 'x');
		ftb_putc(b, ' ');
		ftb_addr(b, e->fn);
		ftb_str(b, "  <- ");
		ftb_addr(b, e->caller);
		ftb_putc(b, '\n');
	}
}

void ftrace_dump_to_q(queue_t *q, unsigned last_n_per_cpu)
{
	struct ftbuf b;
	b.q   = q;
	b.len = 0;

	/* Suppress hook recording on every CPU while we format -- allocb
	 * etc. are instrumented when TRACE=1, and we are about to call
	 * them a lot.  Only this CPU writes its own ring anyway, but
	 * being conservative across all entries keeps the dump trace-
	 * free even if a future caller dumps from an interrupt. */
	for (unsigned c = 0; c < FT_NCPU; c++) in_dump[c] = 1;

	ftb_str(&b, "ftrace: ");
	ftb_str(&b, enabled ? "enabled" : "disabled");
	ftb_str(&b, "  ring_per_cpu=");
	ftb_u(&b, FT_RING_SZ, 0);
	ftb_putc(&b, '\n');

	for (unsigned c = 0; c < FT_NCPU; c++)
		dump_cpu(&b, c, last_n_per_cpu);

	ftb_flush(&b);

	/* M_HANGUP so the reader sees EOF after draining (matches the
	 * one-shot snapshot pattern used by every other /proc file). */
	mblk_t *hup = allocb(1, 0);
	if (hup) {
		hup->b_datap->db_type = M_HANGUP;
		putnext(q, hup);
	}

	for (unsigned c = 0; c < FT_NCPU; c++) in_dump[c] = 0;
}

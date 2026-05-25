#include <stdint.h>

#include "kappara/printk.h"
#include "kappara/timer.h"
#include "kappara/trap.h"

extern char vectors[];

void trap_init(void)
{
	__asm__ volatile (
		"msr	vbar_el1, %0\n"
		"isb\n"
		: : "r"(vectors) : "memory");
}

static const char *vec_name(unsigned id)
{
	static const char *const names[] = {
		"sync_sp0",   "irq_sp0",   "fiq_sp0",   "serror_sp0",
		"sync_spx",   "irq_spx",   "fiq_spx",   "serror_spx",
		"sync_lo64",  "irq_lo64",  "fiq_lo64",  "serror_lo64",
		"sync_lo32",  "irq_lo32",  "fiq_lo32",  "serror_lo32",
	};
	return id < 16 ? names[id] : "??";
}

static const char *ec_name(unsigned ec)
{
	switch (ec) {
	case 0x00: return "unknown";
	case 0x07: return "SIMD/FP trap";
	case 0x15: return "svc (aarch64)";
	case 0x18: return "msr/mrs/sys trap";
	case 0x20: return "instruction abort (lower EL)";
	case 0x21: return "instruction abort (curr EL)";
	case 0x22: return "PC alignment";
	case 0x24: return "data abort (lower EL)";
	case 0x25: return "data abort (curr EL)";
	case 0x26: return "SP alignment";
	case 0x2c: return "FP exception";
	case 0x2f: return "SError";
	case 0x30: return "breakpoint (lower EL)";
	case 0x31: return "breakpoint (curr EL)";
	case 0x32: return "software step (lower EL)";
	case 0x33: return "software step (curr EL)";
	case 0x34: return "watchpoint (lower EL)";
	case 0x35: return "watchpoint (curr EL)";
	case 0x3c: return "brk (aarch64)";
	default:   return "(other)";
	}
}

void trap_dispatch(struct trap_frame *tf, unsigned vec_id)
{
	if (vec_id == VEC_IRQ_SP0 || vec_id == VEC_IRQ_SPX ||
	    vec_id == VEC_IRQ_LO64 || vec_id == VEC_IRQ_LO32) {
		irq_dispatch();
		return;
	}

	unsigned ec = (unsigned)((tf->esr >> 26) & 0x3f);
	unsigned iss = (unsigned)(tf->esr & 0x1ffffff);

	kprintf("\n!! trap: %s  ec=0x%x (%s)  iss=0x%x\n",
		vec_name(vec_id), ec, ec_name(ec), iss);
	kprintf("   elr=0x%016lx  spsr=0x%08lx\n",
		(unsigned long)tf->elr, (unsigned long)tf->spsr);
	kprintf("   esr=0x%08lx  far=0x%016lx\n",
		(unsigned long)tf->esr, (unsigned long)tf->far);

	for (int i = 0; i < 30; i += 2) {
		kprintf("   x%-2d=0x%016lx  x%-2d=0x%016lx\n",
			i,     (unsigned long)tf->x[i],
			i + 1, (unsigned long)tf->x[i + 1]);
	}
	kprintf("   x30=0x%016lx  sp0=0x%016lx\n",
		(unsigned long)tf->x[30], (unsigned long)tf->sp_el0);

	kpanic("unhandled trap");
}

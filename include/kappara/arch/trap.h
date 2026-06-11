/*
 * include/kappara/trap.h -- portable trap entry points
 *
 * trap_init() is the one piece of the exception path that every arch
 * implements: it installs the vector table so the CPU knows where to
 * jump on a fault.  Everything below the #ifdef is the AArch64 trap
 * frame layout and dispatch entry, shared between arch/aarch64
 * C files and arch/aarch64/vectors.S.  ARMv7 has a different frame,
 * defined locally in arch/arm/trap.c.
 */
#ifndef KAPPARA_TRAP_H
#define KAPPARA_TRAP_H

void trap_init(void);

#ifdef __aarch64__

#include <stdint.h>

struct trap_frame {
	uint64_t x[31];
	uint64_t sp_el0;
	uint64_t elr;
	uint64_t spsr;
	uint64_t esr;
	uint64_t far;
};

enum {
	VEC_SYNC_SP0 = 0, VEC_IRQ_SP0,    VEC_FIQ_SP0,    VEC_ERROR_SP0,
	VEC_SYNC_SPX,     VEC_IRQ_SPX,    VEC_FIQ_SPX,    VEC_ERROR_SPX,
	VEC_SYNC_LO64,    VEC_IRQ_LO64,   VEC_FIQ_LO64,   VEC_ERROR_LO64,
	VEC_SYNC_LO32,    VEC_IRQ_LO32,   VEC_FIQ_LO32,   VEC_ERROR_LO32,
};

void trap_dispatch(struct trap_frame *tf, unsigned vec_id);

#endif /* __aarch64__ */

#endif

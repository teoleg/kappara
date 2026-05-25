#ifndef KAPPARA_TRAP_H
#define KAPPARA_TRAP_H

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

void trap_init(void);
void trap_dispatch(struct trap_frame *tf, unsigned vec_id);

#endif

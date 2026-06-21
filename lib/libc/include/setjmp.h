#ifndef _SETJMP_H
#define _SETJMP_H

/*
 * AArch64 jmp_buf layout (matches the asm in setjmp.S):
 *
 *   slot   register
 *   ----   --------
 *   0..11  x19..x30   (callee-saved + LR)
 *   12     sp
 *   13..20 d8..d15    (callee-saved FP regs)
 *
 * 21 doubles = 168 bytes.  We round to 22 slots (176 B) so the
 * struct is 16-aligned, matching the ABI's stack alignment.
 */
typedef long long __jmp_buf[22];

typedef struct { __jmp_buf __jb; } jmp_buf[1];

int  setjmp (jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif /* _SETJMP_H */

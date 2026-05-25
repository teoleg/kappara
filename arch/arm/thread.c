/*
 * arch/arm/thread.c -- ARMv7 per-thread initial frame
 * ===================================================
 *
 * arch_thread_init_frame builds the first synthetic context that
 * context_switch (arch/arm/switch.S) will pop the very first time
 * the scheduler picks a freshly-created thread.
 *
 * The frame layout has to match the asm exactly: 10 words at the
 * top of the thread's kernel stack, in the order context_switch's
 * `pop {r4-r12, lr}` expects.
 *
 *     synthetic frame (high -> low):
 *
 *     +------------------+ <-- stack_top (== stack_base + PAGE_SIZE)
 *     |   lr = trampoline|   +36
 *     +------------------+
 *     |   r12 (= 0)      |   +32
 *     +------------------+
 *     |   r11 = 0        |   +28
 *     |   r10 = 0        |   +24
 *     |   r9  = 0        |   +20
 *     |   r8  = 0        |   +16
 *     |   r7  = 0        |   +12
 *     |   r6  = 0        |   +8
 *     |   r5  = arg      |   +4
 *     |   r4  = fn       |   +0    <-- returned SP
 *     +------------------+
 *
 * Once context_switch pops these, lr == thread_trampoline, and
 * `bx lr` lands there with r4 = fn and r5 = arg ready to go.
 */

#include <stdint.h>

#include "kappara/sched.h"

extern void thread_trampoline(void);

void *arch_thread_init_frame(void *stack_top, void (*fn)(void *), void *arg)
{
	uint32_t *sp = (uint32_t *)stack_top;
	sp -= 10;
	sp[0] = (uint32_t)(uintptr_t)fn;			/* r4  */
	sp[1] = (uint32_t)(uintptr_t)arg;			/* r5  */
	sp[2] = 0;						/* r6  */
	sp[3] = 0;						/* r7  */
	sp[4] = 0;						/* r8  */
	sp[5] = 0;						/* r9  */
	sp[6] = 0;						/* r10 */
	sp[7] = 0;						/* r11 */
	sp[8] = 0;						/* r12 */
	sp[9] = (uint32_t)(uintptr_t)thread_trampoline;		/* lr  */
	return sp;
}

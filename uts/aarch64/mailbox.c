/*
 * arch/aarch64/mailbox.c -- VideoCore mailbox property-tags interface
 * ===================================================================
 *
 * What this file is
 * -----------------
 * The minimal driver that lets the ARM cores send messages to the
 * VideoCore GPU firmware via the "mailbox" interface.  Used by the
 * framebuffer code to ask the GPU to allocate a screen-sized pixel
 * buffer (and, later, to query temperatures, set clock rates, etc).
 *
 * MMIO register map (offsets from PLAT_MBOX_BASE)
 * ----------------------------------------------
 *   0x00  READ   ARM<-VC inbound mailbox.  Each read returns the
 *                most-recent (PA | channel) the GPU has acknowledged.
 *   0x18  STATUS bit 31 (FULL)  set when WRITE would overflow
 *                bit 30 (EMPTY) set when READ would underflow
 *   0x20  WRITE  ARM->VC outbound.  Low 4 bits = channel; rest = PA.
 *
 * Channels
 * --------
 *   0    Power management
 *   1    Framebuffer (legacy single-tag interface; superseded by 8)
 *   8    Property tags (request/response packet) -- THE one we use
 *
 * Property-tags packet format
 * ---------------------------
 *
 *     u32  total_size_bytes              (entire packet incl. these 8 bytes)
 *     u32  req_resp_code                 (0 = request, 0x80000000 = response OK)
 *     [tag1 ...]
 *     [tag2 ...]
 *     ...
 *     u32  end_marker                    (0)
 *
 *   each tag:
 *     u32  tag_id
 *     u32  value_buffer_size_bytes       (max of request/response value sizes)
 *     u32  tag_code                      (0 on request; high bit set + actual
 *                                          length on response)
 *     u32  value[...]                    (in/out parameters)
 *
 * The whole packet must be 16-byte aligned (only the high 28 bits of
 * the address fit into the WRITE register; the low 4 carry the
 * channel number).  We satisfy that with __attribute__((aligned(16)))
 * on the static buffer the caller passes in.
 *
 * Cache maintenance
 * -----------------
 * On real hardware the GPU reads our request straight out of RAM.
 * Any dirty data still in our D-cache won't be seen.  DC CIVAC the
 * whole buffer before WRITE; on the way back, the response was
 * written by the GPU and our D-cache might hold a stale copy, so
 * DC IVAC again before reading.  Use DC CIVAC throughout for
 * simplicity (clean+invalidate covers both directions).  QEMU
 * doesn't actually model caches so this is a no-op there.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/arch/mailbox.h"
#include "kappara/core/printk.h"
#include "platform.h"

#define MBOX_READ	(PLAT_MBOX_BASE + 0x00)
#define MBOX_STATUS	(PLAT_MBOX_BASE + 0x18)
#define MBOX_WRITE	(PLAT_MBOX_BASE + 0x20)

#define MBOX_FULL	(1u << 31)
#define MBOX_EMPTY	(1u << 30)

#define MBOX_CHAN_PROP	8

static inline uint32_t mmio_r32(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void mmio_w32(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

/* Clean+invalidate the cache lines covering [start, start+len). */
static void dc_civac_range(void *start, size_t len)
{
	uintptr_t a   = (uintptr_t)start;
	uintptr_t end = a + len;
	a &= ~63UL;	/* 64-byte cortex-a53 line size */
	for (; a < end; a += 64)
		__asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile ("dsb sy" ::: "memory");
}

int mailbox_property_call(volatile uint32_t *msg, size_t msg_bytes)
{
	if (((uintptr_t)msg & 0xF) != 0)
		return -1;	/* must be 16-byte aligned */

	dc_civac_range((void *)msg, msg_bytes);

	/* Wait for room in the outbound mailbox. */
	while (mmio_r32(MBOX_STATUS) & MBOX_FULL)
		;

	mmio_w32(MBOX_WRITE,
		 ((uint32_t)(uintptr_t)msg & ~0xFu) | MBOX_CHAN_PROP);

	/* Wait for response on the correct channel. */
	for (;;) {
		while (mmio_r32(MBOX_STATUS) & MBOX_EMPTY)
			;
		uint32_t r = mmio_r32(MBOX_READ);
		if ((r & 0xFu) == MBOX_CHAN_PROP) {
			dc_civac_range((void *)msg, msg_bytes);
			/* Response code 0x80000000 = request handled OK. */
			return (msg[1] == 0x80000000u) ? 0 : -1;
		}
		/* else: response for a different channel, keep looking */
	}
}

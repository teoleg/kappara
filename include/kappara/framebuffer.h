/*
 * include/kappara/framebuffer.h -- framebuffer / monitor output
 *
 * Allocates a linear framebuffer through the VideoCore GPU mailbox
 * and exposes it as a flat pixel array.  Works on:
 *   - QEMU `-M raspi3b` (and raspi4b)
 *   - Real Pi 3 family with HDMI monitor attached at boot
 *   - Real Pi 4 family (same VC mailbox protocol)
 *
 * Pixel format is fixed at 32-bit ARGB (one u32 per pixel) for
 * simplicity.  Width/height are negotiated -- if the monitor is
 * smaller we get its native resolution back.
 *
 * Coordinate system: (0,0) is top-left, x increases right, y down.
 */

#ifndef KAPPARA_FRAMEBUFFER_H
#define KAPPARA_FRAMEBUFFER_H

#include <stddef.h>
#include <stdint.h>

struct framebuffer {
	uint32_t	*fb;		/* pixel buffer (32-bpp ARGB)        */
	uint32_t	 width;		/* in pixels                          */
	uint32_t	 height;	/* in pixels                          */
	uint32_t	 pitch;		/* bytes per row (>= width * 4)       */
	uint32_t	 size_bytes;
};

/* Allocate via VC mailbox.  Returns 0 on success, -1 if the GPU
 * declined the request (no monitor, no GPU mem, etc.). */
int                  framebuffer_init(uint32_t want_w, uint32_t want_h);

/* Get the active framebuffer descriptor, or NULL if init failed. */
const struct framebuffer *framebuffer_get(void);

/* Simple drawing primitives.  Bounds-checked. */
void framebuffer_fill (uint32_t color);
void framebuffer_pixel(uint32_t x, uint32_t y, uint32_t color);
void framebuffer_rect (uint32_t x, uint32_t y,
		       uint32_t w, uint32_t h, uint32_t color);

/* DC CVAC the framebuffer back to memory so the GPU sees our writes.
 * Call after a batch of draws.  No-op in QEMU (no real caches);
 * essential on real Pi 3/4 since the framebuffer sits in Normal
 * cacheable RAM and the GPU reads it without going through our
 * D-cache.  Eventually we'll switch the framebuffer's VA to Normal
 * Non-Cacheable and this becomes unnecessary. */
void framebuffer_flush(void);

#endif

/*
 * arch/aarch64/framebuffer.c -- VideoCore framebuffer via mailbox
 * ===============================================================
 *
 * Asks the GPU to allocate a 32-bit ARGB linear framebuffer at a
 * requested resolution, records the resulting pointer + geometry,
 * and exposes simple drawing primitives.  No font rendering yet;
 * that lands in a follow-up commit on top of this one.
 *
 * Mailbox property tags we use
 * ----------------------------
 *   0x00048003  set physical (display) width/height
 *   0x00048004  set virtual (framebuffer) width/height
 *   0x00048005  set depth (bits per pixel)
 *   0x00048006  set pixel order (0=BGR, 1=RGB)
 *   0x00040001  allocate framebuffer (alignment in,
 *                                     base PA + size_bytes out)
 *   0x00040008  get pitch (bytes per row)
 *
 * The packet is a flat u32 array; one allocation does all the setup
 * + the allocate request in a single mailbox call.
 *
 * Pi 3 quirk: the GPU returns the framebuffer's PA in the legacy
 * "VC bus address" namespace, where 0xC0000000 is mapped to RAM PA 0.
 * Mask off the top bit pair to get the ARM-visible PA.  On real Pi 4
 * the protocol is the same; the legacy alias just isn't needed since
 * the GPU returns bus-equivalent addresses already.  Masking is safe
 * on both because the framebuffer always falls in low RAM.
 *
 * Cache attributes
 * ----------------
 * The returned PA falls in our identity-mapped 1 GB region, which
 * we map as Normal cacheable.  That means pixel writes hit the
 * D-cache and don't immediately reach the GPU's view of RAM.  For
 * QEMU this is invisible (no real caches); on real hardware you'd
 * want either a periodic dc_civac of the dirty region or to map
 * the framebuffer as Normal-NC.  For now: simple, correct in QEMU,
 * "good enough to see something" on hardware.  Real fix is part of
 * the framebuffer-console commit that follows.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/font8x8.h"
#include "kappara/framebuffer.h"
#include "kappara/mailbox.h"
#include "kappara/printk.h"

#define MBOX_TAG_SET_PHYS_WH	0x00048003u
#define MBOX_TAG_SET_VIRT_WH	0x00048004u
#define MBOX_TAG_SET_DEPTH	0x00048005u
#define MBOX_TAG_SET_PIXORDER	0x00048006u
#define MBOX_TAG_ALLOCATE_FB	0x00040001u
#define MBOX_TAG_GET_PITCH	0x00040008u

static struct framebuffer fb;
static int fb_ready;

/* Property-tags message buffer; must be 16-byte aligned. */
__attribute__((aligned(16)))
static volatile uint32_t fb_msg[36];

int framebuffer_init(uint32_t want_w, uint32_t want_h)
{
	unsigned i = 0;
	fb_msg[i++] = 0;			/* total bytes, fill in below */
	fb_msg[i++] = 0;			/* request code */

	fb_msg[i++] = MBOX_TAG_SET_PHYS_WH;
	fb_msg[i++] = 8;
	fb_msg[i++] = 0;
	fb_msg[i++] = want_w;
	fb_msg[i++] = want_h;

	fb_msg[i++] = MBOX_TAG_SET_VIRT_WH;
	fb_msg[i++] = 8;
	fb_msg[i++] = 0;
	fb_msg[i++] = want_w;
	fb_msg[i++] = want_h;

	fb_msg[i++] = MBOX_TAG_SET_DEPTH;
	fb_msg[i++] = 4;
	fb_msg[i++] = 0;
	fb_msg[i++] = 32;			/* bits per pixel */

	fb_msg[i++] = MBOX_TAG_SET_PIXORDER;
	fb_msg[i++] = 4;
	fb_msg[i++] = 0;
	fb_msg[i++] = 1;			/* 0 = BGR, 1 = RGB */

	unsigned alloc_tag_idx = i;
	fb_msg[i++] = MBOX_TAG_ALLOCATE_FB;
	fb_msg[i++] = 8;
	fb_msg[i++] = 0;
	fb_msg[i++] = 16;			/* alignment in / addr out */
	fb_msg[i++] = 0;			/* size out */

	unsigned pitch_tag_idx = i;
	fb_msg[i++] = MBOX_TAG_GET_PITCH;
	fb_msg[i++] = 4;
	fb_msg[i++] = 0;
	fb_msg[i++] = 0;			/* pitch out */

	fb_msg[i++] = 0;			/* end marker */
	fb_msg[0] = i * 4;			/* total bytes */

	if (mailbox_property_call(fb_msg, i * 4) < 0) {
		kprintf("framebuffer: mailbox property call failed\n");
		return -1;
	}

	uint32_t fb_pa     = fb_msg[alloc_tag_idx + 3] & 0x3FFFFFFFu;
	uint32_t fb_size   = fb_msg[alloc_tag_idx + 4];
	uint32_t fb_pitch  = fb_msg[pitch_tag_idx + 3];

	if (fb_pa == 0 || fb_size == 0) {
		kprintf("framebuffer: GPU returned zero buffer\n");
		return -1;
	}

	fb.fb         = (uint32_t *)(uintptr_t)fb_pa;
	fb.width      = want_w;
	fb.height     = want_h;
	fb.pitch      = fb_pitch ? fb_pitch : (want_w * 4);
	fb.size_bytes = fb_size;
	fb_ready      = 1;

	kprintf("framebuffer: %ux%u 32-bpp pitch=%u at PA=0x%lx (size=%u)\n",
		fb.width, fb.height, fb.pitch,
		(unsigned long)(uintptr_t)fb.fb, fb.size_bytes);
	return 0;
}

const struct framebuffer *framebuffer_get(void)
{
	return fb_ready ? &fb : NULL;
}

void framebuffer_fill(uint32_t color)
{
	if (!fb_ready) return;
	uint32_t stride = fb.pitch / 4;
	for (uint32_t y = 0; y < fb.height; y++) {
		uint32_t *row = fb.fb + (uint64_t)y * stride;
		for (uint32_t x = 0; x < fb.width; x++)
			row[x] = color;
	}
}

void framebuffer_pixel(uint32_t x, uint32_t y, uint32_t color)
{
	if (!fb_ready || x >= fb.width || y >= fb.height) return;
	uint32_t stride = fb.pitch / 4;
	fb.fb[(uint64_t)y * stride + x] = color;
}

void framebuffer_rect(uint32_t x, uint32_t y,
		      uint32_t w, uint32_t h, uint32_t color)
{
	if (!fb_ready) return;
	uint32_t stride = fb.pitch / 4;
	uint32_t x1 = x + w; if (x1 > fb.width)  x1 = fb.width;
	uint32_t y1 = y + h; if (y1 > fb.height) y1 = fb.height;
	for (uint32_t yy = y; yy < y1; yy++) {
		uint32_t *row = fb.fb + (uint64_t)yy * stride;
		for (uint32_t xx = x; xx < x1; xx++)
			row[xx] = color;
	}
}

void framebuffer_flush(void)
{
	if (!fb_ready) return;
	uintptr_t a   = (uintptr_t)fb.fb;
	uintptr_t end = a + fb.size_bytes;
	a &= ~63UL;
	for (; a < end; a += 64)
		__asm__ volatile ("dc cvac, %0" :: "r"(a) : "memory");
	__asm__ volatile ("dsb sy" ::: "memory");
}

void framebuffer_putc(uint32_t x, uint32_t y, char c,
		      uint32_t fg, uint32_t bg, unsigned scale)
{
	if (!fb_ready || scale == 0) return;
	const uint8_t *g = font8x8[(unsigned char)c & 0x7F];
	uint32_t stride = fb.pitch / 4;

	/* Bail if the cell would walk past the framebuffer.  Cheap
	 * single check vs. per-pixel bounds checks. */
	if (x + 8 * scale > fb.width || y + 8 * scale > fb.height)
		return;

	/* Inline pixel writes -- no framebuffer_rect call per pixel.
	 * For each font row we expand each of its 8 bits into a
	 * scale*scale block; this loop pattern lets the compiler
	 * keep the row pointer in a register and the inner write
	 * straight into the framebuffer. */
	for (unsigned row = 0; row < 8; row++) {
		uint8_t bits = g[row];
		for (unsigned dy = 0; dy < scale; dy++) {
			uint32_t *p = fb.fb
				+ (size_t)(y + row * scale + dy) * stride + x;
			for (unsigned col = 0; col < 8; col++) {
				int on = (bits >> col) & 1;
				if (!on && bg == 0) continue;
				uint32_t color = on ? fg : bg;
				for (unsigned dx = 0; dx < scale; dx++)
					p[col * scale + dx] = color;
			}
		}
	}
}

void framebuffer_puts(uint32_t x, uint32_t y, const char *s,
		      uint32_t fg, uint32_t bg, unsigned scale)
{
	if (!fb_ready || !s) return;
	uint32_t cx = x;
	while (*s) {
		framebuffer_putc(cx, y, *s, fg, bg, scale);
		cx += 8 * scale;
		s++;
	}
}

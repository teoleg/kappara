/*
 * arch/aarch64/fbcon.c -- framebuffer console as a STREAMS driver
 * ===============================================================
 *
 * What this is
 * ------------
 * A character-driver-shaped wrapper around framebuffer_putc that
 * turns the linear pixel framebuffer into a scrolling text console
 * accessible at /dev/fbcon.  Writing bytes to the device draws the
 * corresponding glyphs at the current cursor position; \n moves to
 * the next line, \r returns to column 0, \b erases the previous
 * character, and any other printable byte advances the cursor one
 * cell.  When the cursor reaches the bottom edge the screen scrolls
 * up by one character row.
 *
 *   write(fd_fbcon, "hello\n", 6)
 *       -> stream_write -> putnext(head_wq)
 *       -> fbcon_wq_putp walks the mblk
 *           for each byte: fbcon_putc -> framebuffer_putc + cursor
 *       -> framebuffer_flush so the GPU sees fresh pixels
 *       -> return 6
 *
 * No read side: the driver is write-only for now (rq.putp just
 * forwards anything that arrives, which never happens since nothing
 * puts to a /dev/fbcon read queue).
 *
 * Cell geometry
 * -------------
 *   FBCON_SCALE = 2  -> each font pixel is 2x2, so glyphs are 16x16.
 *                       1024x768 -> 64 columns x 48 rows.
 *
 * Layout choices left for later
 * -----------------------------
 *   * No colours yet -- white on black.  fg/bg as escape sequences
 *     or an ioctl is a natural next step.
 *   * Cursor isn't drawn; pixels just go where we last wrote.
 *   * Scroll is a brute-force pixel copy.  Fine at 1024x768; will
 *     get expensive at higher resolutions.
 *   * The boot splash drawn in kmain stays put until the first
 *     write -- subsequent text just paints over it.
 */

#include <stddef.h>
#include <stdint.h>

#include "kappara/framebuffer.h"
#include "kappara/printk.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"

#define FBCON_SCALE	2
#define FBCON_CW	(8 * FBCON_SCALE)	/* cell width  in pixels */
#define FBCON_CH	(8 * FBCON_SCALE)	/* cell height in pixels */
#define FBCON_FG	0xffffffffu		/* white */
#define FBCON_BG	0xff000000u		/* black */

static struct {
	uint32_t cx;	/* cursor x in pixels (top-left of next cell) */
	uint32_t cy;	/* cursor y in pixels                          */
} fbcon;

static void fbcon_scroll_one_row(void)
{
	const struct framebuffer *fb = framebuffer_get();
	if (!fb) return;
	uint32_t stride = fb->pitch / 4;
	uint32_t rows   = fb->height - FBCON_CH;
	for (uint32_t y = 0; y < rows; y++) {
		uint32_t *dst = fb->fb + (size_t)y * stride;
		uint32_t *src = fb->fb + (size_t)(y + FBCON_CH) * stride;
		for (uint32_t x = 0; x < fb->width; x++)
			dst[x] = src[x];
	}
	framebuffer_rect(0, fb->height - FBCON_CH,
			 fb->width, FBCON_CH, FBCON_BG);
}

static void fbcon_putc(char c)
{
	const struct framebuffer *fb = framebuffer_get();
	if (!fb) return;

	switch (c) {
	case '\n':
		fbcon.cx  = 0;
		fbcon.cy += FBCON_CH;
		break;
	case '\r':
		fbcon.cx = 0;
		break;
	case '\b':
		if (fbcon.cx >= FBCON_CW) {
			fbcon.cx -= FBCON_CW;
			framebuffer_rect(fbcon.cx, fbcon.cy,
					 FBCON_CW, FBCON_CH, FBCON_BG);
		}
		break;
	default:
		framebuffer_putc(fbcon.cx, fbcon.cy, c,
				 FBCON_FG, FBCON_BG, FBCON_SCALE);
		fbcon.cx += FBCON_CW;
		if (fbcon.cx + FBCON_CW > fb->width) {
			fbcon.cx  = 0;
			fbcon.cy += FBCON_CH;
		}
		break;
	}

	if (fbcon.cy + FBCON_CH > fb->height) {
		fbcon_scroll_one_row();
		fbcon.cy = fb->height - FBCON_CH;
	}
}

/* ---- STREAMS plumbing ------------------------------------------------ */

static int fbcon_wq_putp(queue_t *q, mblk_t *mp)
{
	(void)q;
	for (mblk_t *m = mp; m; m = m->b_cont) {
		for (unsigned char *p = m->b_rptr; p < m->b_wptr; p++)
			fbcon_putc((char)*p);
	}
	framebuffer_flush();
	freemsg(mp);
	return 0;
}

static int fbcon_rq_putp(queue_t *q, mblk_t *mp)
{
	return putnext(q, mp);
}

static struct module_info fbcon_minfo = {
	.mi_idnum  = 202,
	.mi_idname = "fbcon",
	.mi_minpsz = 0,
	.mi_maxpsz = 4096,
	.mi_hiwat  = 16384,
	.mi_lowat  = 8192,
};

static struct qinit fbcon_rinit = {
	.qi_putp = fbcon_rq_putp, .qi_minfo = &fbcon_minfo
};
static struct qinit fbcon_winit = {
	.qi_putp = fbcon_wq_putp, .qi_minfo = &fbcon_minfo
};

struct streamtab fbcon_streamtab = {
	.st_rdinit = &fbcon_rinit,
	.st_wrinit = &fbcon_winit,
};

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

#include "kappara/fbcon.h"
#include "kappara/framebuffer.h"
#include "kappara/printk.h"
#include "kappara/stream_head.h"
#include "kappara/streams.h"

/*
 * FBCON_SCALE = 1 keeps the per-character cost low enough that
 * QEMU TCG (which emulates every pixel write as a memory access
 * with no caching) can render a kprintf-heavy boot at human speed.
 * On real Pi hardware, raising this to 2 makes the text much easier
 * to read on a TV.  TODO: pick at runtime based on framebuffer
 * geometry and target.
 */
#define FBCON_SCALE	1
#define FBCON_CW	(8 * FBCON_SCALE)	/* cell width  in pixels */
#define FBCON_CH	(8 * FBCON_SCALE)	/* cell height in pixels */
#define FBCON_FG	0xffffffffu		/* white */
#define FBCON_BG	0xff000000u		/* black */

static struct {
	uint32_t cx;	/* cursor x in pixels (top-left of next cell) */
	uint32_t cy;	/* cursor y in pixels                          */
} fbcon;

/*
 * Dirty-rectangle tracking.  We only need a vertical span because
 * almost everything we draw spans the full width of a character row.
 * fbcon_putc updates the span around each glyph cell it touches;
 * fbcon_scroll_one_row marks the whole framebuffer dirty since it
 * copies pixels everywhere.  fbcon_tee_flush dc-cvacs only the rows
 * in that span, not the whole 3 MB.
 */
static uint32_t fbcon_dirty_y_lo = 0xffffffffu;
static uint32_t fbcon_dirty_y_hi = 0;

static void fbcon_mark_dirty(uint32_t y, uint32_t h)
{
	uint32_t end = y + h;
	if (y   < fbcon_dirty_y_lo) fbcon_dirty_y_lo = y;
	if (end > fbcon_dirty_y_hi) fbcon_dirty_y_hi = end;
}

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
	fbcon_mark_dirty(0, fb->height);
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
		fbcon_mark_dirty(fbcon.cy, FBCON_CH);
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

/* ---- "tee" entry points used by printk + /dev/console -------------- */

/* Tee state: we accumulate dirty writes and let the caller (kprintf,
 * console_wq_putp) decide when to push them to the GPU via
 * fbcon_tee_flush.  Otherwise every kprintf char triggers a 3 MB
 * dc cvac sweep -- death by a thousand cuts in QEMU TCG. */
static int fbcon_tee_dirty;

/*
 * Runtime knob.  Off by default: the boot splash is drawn once and
 * left alone.  When the QEMU display window is visible the host's
 * 30-60 Hz refresh thread reads the entire 3 MB framebuffer every
 * frame, and that competes with the guest's per-kprintf
 * pixel-write + dc cvac loop to the point where the shell appears
 * to freeze.  Minimizing the window stops the host-side reads and
 * "fixes" it -- but the real fix is just not to write the kernel
 * log to the framebuffer in the first place.  /dev/fbcon writes
 * via the STREAMS driver still work for anyone who wants text on
 * screen, and the boot splash stays put because it's drawn through
 * the framebuffer_* primitives, not through the tee.
 */
static int fbcon_tee_enabled;

void fbcon_tee_enable(int on)
{
	fbcon_tee_enabled = on;
}

void fbcon_putc_tee(char c)
{
	if (!fbcon_tee_enabled || !framebuffer_get())
		return;
	fbcon_putc(c);
	fbcon_tee_dirty = 1;
}

/*
 * Dirty-rectangle flush.  Only the rows we actually touched since
 * the last flush get a dc cvac sweep -- typically one or two text
 * rows (~16 cache lines each) per kprintf, not all 49k lines of
 * the 3 MB framebuffer.  Reduces boot-time fb cache maintenance
 * cost by roughly 1000x and brings the shell prompt back from
 * "minutes in QEMU TCG" to "instant".
 *
 * The flush is essential on real Pi 3/4 silicon so the GPU sees
 * fresh pixels; QEMU doesn't model caches and would work without
 * it, but we keep the sweep so the same kernel image is correct on
 * both targets.
 */
void fbcon_tee_flush(void)
{
	if (!fbcon_tee_dirty)
		return;
	const struct framebuffer *fb = framebuffer_get();
	if (fb && fbcon_dirty_y_hi > fbcon_dirty_y_lo) {
		uint32_t y0 = fbcon_dirty_y_lo;
		uint32_t y1 = fbcon_dirty_y_hi;
		if (y1 > fb->height) y1 = fb->height;
		uintptr_t row_start = (uintptr_t)fb->fb
			+ (size_t)y0 * fb->pitch;
		uintptr_t row_end   = (uintptr_t)fb->fb
			+ (size_t)y1 * fb->pitch;
		row_start &= ~63UL;
		for (; row_start < row_end; row_start += 64)
			__asm__ volatile ("dc cvac, %0"
					  :: "r"(row_start) : "memory");
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	fbcon_dirty_y_lo = 0xffffffffu;
	fbcon_dirty_y_hi = 0;
	fbcon_tee_dirty  = 0;
}

void fbcon_init_cursor(uint32_t y)
{
	fbcon.cx = 0;
	fbcon.cy = y;
}

/* ---- Phase 7: cell-grid renderer driven by struct vt ------------------ */

#include "kappara/vt.h"

/* Map a VT color (0..15 with VT_COLOR_BRIGHT bit) to a 32-bit ARGB
 * value.  The standard 16-color VGA palette -- close enough to xterm
 * defaults that programs that hardcode the basic 8 colors look right.
 * VT_COLOR_DEFAULT picks "light grey" for fg and "black" for bg, the
 * xterm convention. */
static uint32_t fbcon_palette_fg[16] = {
	0xff000000u, 0xffaa0000u, 0xff00aa00u, 0xffaa5500u,
	0xff0000aau, 0xffaa00aau, 0xff00aaaau, 0xffaaaaaau,
	0xff555555u, 0xffff5555u, 0xff55ff55u, 0xffffff55u,
	0xff5555ffu, 0xffff55ffu, 0xff55ffffu, 0xffffffffu,
};

static uint32_t fbcon_argb_fg(uint8_t c)
{
	if (c == VT_COLOR_DEFAULT) return 0xffaaaaaau;	/* light grey */
	return fbcon_palette_fg[c & 0xf];
}

static uint32_t fbcon_argb_bg(uint8_t c)
{
	if (c == VT_COLOR_DEFAULT) return 0xff000000u;	/* black */
	return fbcon_palette_fg[c & 0xf];
}

/* Render one cell at grid position (row, col).  Caller is responsible
 * for the surrounding dc cvac flush. */
static void fbcon_render_cell(const struct vt_cell *cell, int row, int col)
{
	const struct framebuffer *fb = framebuffer_get();
	if (!fb) return;
	uint32_t fg = fbcon_argb_fg(cell->fg);
	uint32_t bg = fbcon_argb_bg(cell->bg);
	if (cell->flags & VT_FLAG_REVERSE) {
		uint32_t t = fg; fg = bg; bg = t;
	}
	uint32_t x = (uint32_t)col * FBCON_CW;
	uint32_t y = (uint32_t)row * FBCON_CH;
	framebuffer_putc(x, y, (char)cell->ch, fg, bg, FBCON_SCALE);
}

/* Repaint the entire cell grid for the given VT.  The grid is laid
 * out starting at (0,0) of the framebuffer; on a 1024x768 display
 * with FBCON_SCALE=1 the grid is 80x24 chars = 640x192 pixels and
 * leaves the bottom 576 rows untouched for the kprintf/splash band
 * the existing tee mode uses. */
void fbcon_render_vt(const struct vt *v)
{
	if (!framebuffer_get() || !v) return;
	/* Clear the grid background to BG first so cells that have
	 * never been written (or just got blanked) come out as the
	 * default colour instead of stale splash pixels. */
	framebuffer_rect(0, 0,
	                 (uint32_t)VT_COLS * FBCON_CW,
	                 (uint32_t)VT_ROWS * FBCON_CH,
	                 0xff000000u);
	for (int r = 0; r < VT_ROWS; r++) {
		for (int c = 0; c < VT_COLS; c++) {
			fbcon_render_cell(&v->cells[r][c], r, c);
		}
	}
	fbcon_mark_dirty(0, (uint32_t)VT_ROWS * FBCON_CH);
	fbcon_tee_flush();
}

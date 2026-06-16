/*
 * uts/virt/framebuffer.c -- no-framebuffer stubs for QEMU virt
 *
 * The virt machine has no VideoCore GPU and no display.  Every
 * framebuffer entry point is a no-op so the shared main.c can call
 * framebuffer_init() / framebuffer_get() unchanged and fall through
 * to its no-GPU code paths -- which print to UART only, which is
 * exactly what we want with `-nographic`.
 */

#include <stdint.h>

#include "kappara/arch/framebuffer.h"

int framebuffer_init(uint32_t want_w, uint32_t want_h)
{
	(void)want_w; (void)want_h;
	return -1;	/* no GPU; main.c falls through to UART-only path */
}

const struct framebuffer *framebuffer_get(void)
{
	return 0;
}

void framebuffer_fill (uint32_t color) { (void)color; }
void framebuffer_pixel(uint32_t x, uint32_t y, uint32_t color)
	{ (void)x; (void)y; (void)color; }
void framebuffer_rect (uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t color)
	{ (void)x; (void)y; (void)w; (void)h; (void)color; }
void framebuffer_putc (uint32_t x, uint32_t y, char c,
                       uint32_t fg, uint32_t bg, unsigned scale)
	{ (void)x; (void)y; (void)c; (void)fg; (void)bg; (void)scale; }
void framebuffer_puts (uint32_t x, uint32_t y, const char *s,
                       uint32_t fg, uint32_t bg, unsigned scale)
	{ (void)x; (void)y; (void)s; (void)fg; (void)bg; (void)scale; }
void framebuffer_flush(void) {}

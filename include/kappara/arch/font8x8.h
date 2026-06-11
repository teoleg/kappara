/*
 * include/kappara/font8x8.h -- 8x8 ASCII font data
 *
 * One byte per row, eight rows per glyph, bit 0 = leftmost pixel.
 * Indexed by ASCII code; entries outside 0x20..0x7E are all-zero
 * (render as blanks).  Used by framebuffer_putc / framebuffer_puts.
 */
#ifndef KAPPARA_FONT8X8_H
#define KAPPARA_FONT8X8_H

#include <stdint.h>

extern const uint8_t font8x8[128][8];

#endif

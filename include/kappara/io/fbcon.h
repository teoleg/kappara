/*
 * include/kappara/fbcon.h -- framebuffer console "tee" entry points
 *
 * The /dev/fbcon STREAMS driver is the user-facing way to write to
 * the framebuffer.  This header exposes a smaller back door used by
 * printk and the /dev/console driver to ALSO mirror kernel-side
 * output onto the screen, so the boot log appears on the HDMI
 * monitor and not just over UART.
 *
 *   fbcon_putc_tee(c)
 *     Draws c at the current fbcon cursor, but only if the
 *     framebuffer has been initialised.  A no-op otherwise (so it's
 *     safe to call from early-boot kprintf before the framebuffer
 *     is up).  Flushes the relevant cache lines on '\n'.
 *
 *   fbcon_init_cursor(y)
 *     Sets the fbcon cursor position so subsequent writes land
 *     below any pre-painted artwork (the boot splash, etc.).  Call
 *     from kmain after framebuffer_init + splash drawing.
 *
 * On ARMv7 (no framebuffer support yet) both calls compile to
 * inline no-ops.
 */
#ifndef KAPPARA_FBCON_H
#define KAPPARA_FBCON_H

#include <stdint.h>

struct vt;	/* fwd; defined in kappara/vt.h */

#ifdef __aarch64__
void fbcon_putc_tee   (char c);
void fbcon_tee_flush  (void);
void fbcon_init_cursor(uint32_t y);
/* Toggle the kprintf-to-framebuffer mirror.  Off by default --
 * the splash still renders (it goes through framebuffer_* directly,
 * not the tee), but the kernel log stays on UART so QEMU's display
 * refresh thread doesn't fight per-character fbcon writes for the
 * same 3 MB region. */
void fbcon_tee_enable (int on);
/* Phase 7: paint a struct vt's full cell grid onto the framebuffer.
 * 80x24 cells at FBCON_CW x FBCON_CH pixels each, top-left aligned.
 * Mapped via the standard 16-color VGA palette; VT_COLOR_DEFAULT
 * resolves to xterm's light-grey-on-black.  Caller decides when to
 * paint -- typically tty_switch (full repaint on switch) and the
 * tty driver's wq_putp (incremental redraw on active tty's writes).
 * No-op when no framebuffer is up. */
void fbcon_render_vt  (const struct vt *v);
#else
static inline void fbcon_putc_tee   (char c)        { (void)c; }
static inline void fbcon_tee_flush  (void)          { }
static inline void fbcon_init_cursor(uint32_t y)    { (void)y; }
static inline void fbcon_tee_enable (int on)        { (void)on; }
static inline void fbcon_render_vt  (const struct vt *v) { (void)v; }
#endif

#endif

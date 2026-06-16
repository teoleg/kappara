/*
 * uts/virt/fbcon.c -- no-framebuffer fbcon stubs for QEMU virt
 *
 * Symmetric with uts/virt/framebuffer.c: virt has no display, so the
 * "tee to screen" path is uniformly a no-op.  Kernel log still
 * reaches the user via the PL011 UART (`-nographic` redirects it to
 * stdio), and tty switches still update the in-memory vt grid.  We
 * just skip the actual pixel paint.
 *
 * A zeroed `fbcon_streamtab` keeps stream_head.c's
 * streams_register("fbcon", ...) and cdev_register linkage happy;
 * stream_open on /dev/fbcon will fail the qopen NULL deref, which is
 * exactly what should happen on a board with no framebuffer.
 */

#include <stdint.h>

#include "kappara/io/fbcon.h"
#include "kappara/io/streams.h"

void fbcon_putc_tee   (char c)              { (void)c; }
void fbcon_tee_flush  (void)                { }
void fbcon_init_cursor(uint32_t y)          { (void)y; }
void fbcon_tee_enable (int on)              { (void)on; }
void fbcon_render_vt  (const struct vt *v)  { (void)v; }

struct streamtab fbcon_streamtab = { 0 };

/*
 * include/kappara/io/tty.h -- multi-minor TTY (virtual console) driver
 *
 * Phase 3 of the virtual-console roadmap.  Each minor `i` in the
 * range 0..NTTY-1 owns one `struct tty_minor`: an instance of the
 * phase-1 VT100 emulator + a backref to the open `stdata` for
 * /dev/tty<i>.
 *
 * Writes that come down the tty stream stack (sys_write -> head ->
 * possibly ldterm -> driver) feed bytes through that tty's `vt`
 * cell buffer.  If the tty is the currently-active one, the same
 * bytes are also written straight to UART so the user sees them
 * live; if it's inactive, only the cell buffer updates -- the
 * bytes show up next time the user switches to it via
 * `tty_switch(i)`, which clears the host terminal and replays the
 * inactive tty's cell buffer as ANSI escapes.
 *
 * Reads aren't routed yet -- there's still only one input source
 * (UART) and `uart_rx_main` still feeds `console_active`.  Phase 6
 * (multi-shell spawn) and the corresponding `uart_rx_main` change
 * will route UART rx to the active tty's read queue and wire
 * Ctrl-A N as the switch keystroke.
 */
#ifndef KAPPARA_TTY_H
#define KAPPARA_TTY_H

#include <stdint.h>

#include "kappara/io/vt.h"

/* NTTY: total tty minors that exist as /dev/tty<i> chrdevs.  The
 * first NTTY_VISIBLE (tty0..tty3) are local consoles routed through
 * the UART; the remaining ones are "remote ttys" -- their output
 * goes through tty_minor.output_hook instead of UART, and their
 * input is pushed via tty_remote_feed_byte().  Today tty4 is the
 * single telnet-console slot (telnetd takes over its hook on each
 * accept and bridges the bytes to/from a TCP session). */
#define NTTY_VISIBLE	4
#define NTTY		5

struct stdata;	/* fwd; defined in kappara/io/stream_head.h */

struct tty_minor {
	struct vt        vt;
	struct stdata   *sd;
	int              minor;
	/* When non-NULL, tty_drv_wq_putp routes each output byte
	 * through this hook instead of touching the UART.  Used by
	 * telnetd to bridge a remote-tty's writes into a TCP socket.
	 * Set/cleared via tty_set_output_hook. */
	void           (*output_hook)(int minor, const void *buf,
	                              unsigned len, void *arg);
	void            *output_hook_arg;
};

/* Install (or clear with fn=NULL) an output redirect on this minor.
 * One slot per minor; the next call overwrites.  Reads stay routed
 * through the normal head sd_rq -- the input side is fed via
 * tty_remote_feed_byte() from the bridge. */
void tty_set_output_hook(int minor,
                         void (*fn)(int, const void *, unsigned, void *),
                         void *arg);

/* Push one byte into the remote tty's driver read queue, exactly as
 * if it arrived from a UART.  ldterm + the head pick it up the same
 * way they do for tty0..3. */
void tty_remote_feed_byte(int minor, unsigned char c);

/* Register the tty driver + create /dev/tty0..tty<NTTY-1>.  Also
 * initialises tty_minors[i].vt for each.  Called once from
 * streams_head_init. */
void tty_init(void);

/* Switch the active tty.  Sends CSI 2J to clear the host terminal
 * and replays tty[i]'s cell buffer as ANSI.  Cursor lands at the
 * VT's current (row, col).  No-op if `i` is OOB or already active. */
void tty_switch(int i);

/* Currently-active tty's minor.  0 at boot. */
int  tty_active(void);

/* Per-minor driver-side read queue, or NULL if /dev/tty<minor> is
 * not open yet.  uart_rx_main uses this to push received UART bytes
 * into the active tty's stream stack -- they flow up through ldterm
 * and land in the head's read queue for sys_read. */
struct queue *tty_drv_rq(int minor);

/* SVR4 controlling-tty foreground process group for /dev/tty<i>.
 * tty_signal_fg_pgrp delivers `sig` to every kthread whose t_pgrp
 * matches that tty's fg_pgrp; if fg_pgrp is 0 it falls back to the
 * stream head's sd_last_reader (the pre-session-model behaviour
 * uart_rx_main has used since the start).  Returns the number of
 * recipients (0 = no one received it). */
void tty_set_fg_pgrp(int minor, unsigned pgrp);
unsigned tty_fg_pgrp(int minor);
int  tty_signal_fg_pgrp(int minor, unsigned sig);

/* Boot-time self-test -- writes bytes to tty 1 (inactive), checks
 * the cell buffer, switches, switches back. */
void tty_selftest(void);

#endif

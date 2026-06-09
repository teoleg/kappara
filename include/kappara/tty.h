/*
 * include/kappara/tty.h -- multi-minor TTY (virtual console) driver
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

#include "kappara/vt.h"

#define NTTY	4

struct stdata;	/* fwd; defined in kappara/stream_head.h */

struct tty_minor {
	struct vt        vt;
	struct stdata   *sd;
	int              minor;
};

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

/* Boot-time self-test -- writes bytes to tty 1 (inactive), checks
 * the cell buffer, switches, switches back. */
void tty_selftest(void);

#endif

/*
 * include/kappara/vt.h -- VT100/ANSI terminal emulator
 *
 * A self-contained terminal emulator with per-VT cell-buffer state.
 * Takes a stream of bytes from a user program (writes to /dev/ttyN)
 * and maintains an 80x24 grid of (char, fg, bg, flags) cells plus a
 * cursor.  The renderer (fbcon, eventually) walks the cells; the
 * UART switching path drives a full-screen repaint by re-emitting
 * the cells as ANSI sequences over the wire.
 *
 * Phase 1 of the virtual-console roadmap.  See docs/ARCHITECTURE.md
 * for the bigger picture.
 *
 * Sequences supported (matches xterm/VT100 minimum, expanded over
 * subsequent phases):
 *
 *   Plain bytes      : printable -> put at cursor + advance.
 *   Control bytes    : BEL (\007, flag), BS (\010), HT (\011),
 *                      LF (\012, with scroll), CR (\015).
 *   ESC + final      : ESC c  -- reset to initial state (RIS)
 *                      ESC D  -- index   (cursor down + scroll)
 *                      ESC M  -- reverse-index (up + scroll-down)
 *                      ESC 7  -- save cursor   (DECSC)
 *                      ESC 8  -- restore cursor (DECRC)
 *                      ESC E  -- next-line
 *   CSI <params> ... : ESC [ Ps ; Ps ; ... <final>
 *     A B C D        -- cursor up/down/right/left  (CUU/CUD/CUF/CUB)
 *     E F            -- next line / prev line       (CNL/CPL)
 *     G              -- cursor horizontal absolute  (CHA)
 *     H f            -- cursor position             (CUP/HVP)
 *     J              -- erase in display (0/1/2)    (ED)
 *     K              -- erase in line    (0/1/2)    (EL)
 *     L M            -- insert/delete lines         (IL/DL)
 *     @ P            -- insert/delete chars         (ICH/DCH)
 *     S T            -- scroll up/down              (SU/SD)
 *     m              -- SGR: 0/1/4/7/22/24/27/30-37/39/40-47/49/90-97/100-107
 *     r              -- set scroll region DECSTBM
 *     s / u          -- ANSI save/restore cursor    (SCP/RCP)
 *     ? h / ? l      -- DEC private mode set/reset
 *                       ?25 cursor visibility
 *                       ?7  autowrap (DECAWM)
 *
 * The emulator is byte-stream-clean: it never blocks, never
 * allocates, never calls back into the kernel.  All state lives in
 * struct vt.
 */
#ifndef KAPPARA_VT_H
#define KAPPARA_VT_H

#include <stdint.h>
#include <stddef.h>

#define VT_COLS	80
#define VT_ROWS	24

/* ANSI color slots 0..15 (8 standard + 8 bright).  Stored verbatim
 * in cell.fg / cell.bg so the renderer can map them to its native
 * palette without re-parsing.  Default = 0xFF means "use the
 * terminal default" (fg=light grey, bg=black in xterm). */
#define VT_COLOR_BLACK		0
#define VT_COLOR_RED		1
#define VT_COLOR_GREEN		2
#define VT_COLOR_YELLOW		3
#define VT_COLOR_BLUE		4
#define VT_COLOR_MAGENTA	5
#define VT_COLOR_CYAN		6
#define VT_COLOR_WHITE		7
#define VT_COLOR_BRIGHT		8	/* OR into the above for hi-intensity */
#define VT_COLOR_DEFAULT	0xff

/* Cell flag bits.  All optional; renderer may collapse some onto
 * intensity if the underlying display can't show them. */
#define VT_FLAG_BOLD		0x01
#define VT_FLAG_UNDERLINE	0x02
#define VT_FLAG_REVERSE		0x04

struct vt_cell {
	uint8_t ch;	/* printable ASCII; multi-byte UTF-8 deferred to
			 * a later phase along with the renderer-side
			 * codepoint plumbing. */
	uint8_t fg;	/* VT_COLOR_* */
	uint8_t bg;	/* VT_COLOR_* */
	uint8_t flags;	/* VT_FLAG_* */
};

/* Parser state. */
enum vt_state {
	VT_S_GROUND = 0,	/* normal bytes */
	VT_S_ESC,		/* saw ESC, waiting for next byte */
	VT_S_CSI,		/* saw ESC [, collecting params */
};

/* Maximum CSI parameters we collect.  Any beyond this are silently
 * discarded -- real terminals top out at 16; xterm at 30.  SGR is
 * the only sequence that ever uses more than 4 in practice. */
#define VT_PARAMS_MAX	16

struct vt {
	struct vt_cell cells[VT_ROWS][VT_COLS];

	/* Cursor (0-indexed) and the saved cursor for DECSC/DECRC. */
	int row;
	int col;
	int saved_row;
	int saved_col;

	/* Current attributes used when writing a plain char. */
	uint8_t cur_fg;
	uint8_t cur_bg;
	uint8_t cur_flags;
	uint8_t saved_fg;
	uint8_t saved_bg;
	uint8_t saved_flags;

	/* Scroll region: rows [scroll_top..scroll_bot] inclusive.
	 * Default = full screen (0..VT_ROWS-1). */
	int scroll_top;
	int scroll_bot;

	/* Mode flags. */
	int autowrap;		/* DECAWM, default 1 */
	int cursor_visible;	/* DECTCEM, default 1 */
	int wrap_pending;	/* "we just wrote the rightmost column;
				 * the NEXT printable byte triggers wrap"
				 * -- xterm-style "phantom" cursor, see
				 * XTerm: vttests/vtwrap.sh */

	/* Parser state + CSI parameter collection. */
	enum vt_state state;
	int params[VT_PARAMS_MAX];
	int n_params;
	int csi_private;	/* 1 if we saw '?' after ESC [ */

	/* BEL marker -- set when a 0x07 was eaten, cleared by
	 * vt_take_bell so the driver can drive an audible/visible
	 * beep without the emulator needing a callback. */
	int bell_pending;

	/* Dirty-row range since last vt_take_dirty().  Renderers
	 * (fbcon, the UART switch repaint) ask "what rows changed?"
	 * and only repaint those.  Range is inclusive [top..bot];
	 * top > bot means "nothing dirty".  vt_init starts in the
	 * empty state. */
	int dirty_top;
	int dirty_bot;
};

/* Initialise vt to a blank 80x24 grid, cursor at (0,0), default
 * attributes, full-screen scroll region. */
void vt_init(struct vt *v);

/* Feed one byte through the parser; updates state.  Safe to call
 * from any context (no locks taken). */
void vt_feed(struct vt *v, uint8_t b);

/* Batched feed -- equivalent to a loop of vt_feed but lets the
 * caller pass an mblk's b_rptr..b_wptr range without writing the
 * loop themselves. */
void vt_feed_bytes(struct vt *v, const uint8_t *buf, size_t len);

/* Consume a pending bell (returns 1 once per BEL byte fed, 0
 * otherwise). */
int  vt_take_bell(struct vt *v);

/* Fetch + clear the dirty-row range.  Returns 1 if anything was
 * dirty and stores the inclusive [top..bot] in *topbot; returns
 * 0 if nothing changed since the last call (and leaves the
 * outputs untouched).  Renderers loop r=top..bot. */
int  vt_take_dirty(struct vt *v, int *top, int *bot);

/* Force the next vt_take_dirty to report the full screen.  Used
 * by tty_switch when the renderer needs to repaint everything
 * even if nothing's been written since the last fetch. */
void vt_mark_all_dirty(struct vt *v);

/* Read accessor for renderers.  Returns NULL if (row, col) is OOB. */
const struct vt_cell *vt_cell_at(const struct vt *v, int row, int col);

/* Boot-time self-test -- feeds a known sequence and verifies the
 * cell buffer matches expected via kprintf.  Cheap (one pass over
 * a ~80-byte string); runs once from kmain. */
void vt_selftest(void);

#endif

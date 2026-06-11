/*
 * kernel/vt.c -- VT100/ANSI terminal emulator
 *
 * The byte-stream-clean state machine described in the header.
 * Drives an 80x24 (struct vt_cell) grid.  No allocations, no locks,
 * no callbacks -- pure state mutation, suitable for calling from
 * any context (sh_rq_putp, ldterm, switch repaint, ...).
 *
 * Phase 1 of the virtual-console roadmap.
 *
 * Implementation notes
 * --------------------
 * Cursor model: rows 0..VT_ROWS-1, cols 0..VT_COLS-1.  After writing
 * a char at col VT_COLS-1 we DON'T advance the column to VT_COLS
 * (that would put it OOB); instead we set wrap_pending=1.  The next
 * printable byte first does CR+LF then writes itself.  This is
 * xterm's "phantom cursor" behaviour and is required for terminals
 * that line-wrap correctly without producing a spurious blank
 * column when the writer ends a line on the right margin.
 *
 * Scroll region: cursor moves and erase ops respect (scroll_top,
 * scroll_bot).  Newlines that step past scroll_bot scroll the
 * region only; rows outside it are immutable.  This is what makes
 * vi/less able to scroll a status bar fixed.
 *
 * SGR: a single CSI m may carry many parameters
 * ("\x1b[0;1;31;40m" = reset + bold + red + black bg).  We walk the
 * params array; each verb mutates cur_fg/cur_bg/cur_flags, which
 * the next printable char picks up.
 */

#include "kappara/io/vt.h"
#include "kappara/core/printk.h"

/* ---- cell-buffer primitives ------------------------------------------- */

static void vt_dirty_row(struct vt *v, int row)
{
	if (row < 0 || row >= VT_ROWS) return;
	if (row < v->dirty_top) v->dirty_top = row;
	if (row > v->dirty_bot) v->dirty_bot = row;
}

static void vt_dirty_range(struct vt *v, int top, int bot)
{
	if (top < 0)          top = 0;
	if (bot >= VT_ROWS)   bot = VT_ROWS - 1;
	if (top > bot)        return;
	if (top < v->dirty_top) v->dirty_top = top;
	if (bot > v->dirty_bot) v->dirty_bot = bot;
}

static void vt_clear_row(struct vt *v, int row, int col_start, int col_end)
{
	if (row < 0 || row >= VT_ROWS) return;
	if (col_start < 0)        col_start = 0;
	if (col_end > VT_COLS)    col_end = VT_COLS;
	if (col_end <= col_start) return;
	for (int c = col_start; c < col_end; c++) {
		v->cells[row][c].ch    = ' ';
		v->cells[row][c].fg    = v->cur_fg;
		v->cells[row][c].bg    = v->cur_bg;
		v->cells[row][c].flags = 0;
	}
	vt_dirty_row(v, row);
}

static void vt_clear_region(struct vt *v, int r_start, int r_end)
{
	for (int r = r_start; r < r_end && r < VT_ROWS; r++)
		vt_clear_row(v, r, 0, VT_COLS);
}

/* Move rows[from..from+count) one step down or up inside the scroll
 * region; the vacated row gets blanked.  Both directions kept in
 * one helper because LF (scroll up) and RI (scroll down) want the
 * same logic. */
static void vt_scroll_up(struct vt *v, int n)
{
	if (n <= 0) return;
	int top = v->scroll_top, bot = v->scroll_bot;
	if (bot < top) return;
	int height = bot - top + 1;
	if (n > height) n = height;

	for (int r = top; r + n <= bot; r++) {
		for (int c = 0; c < VT_COLS; c++)
			v->cells[r][c] = v->cells[r + n][c];
	}
	for (int r = bot - n + 1; r <= bot; r++)
		vt_clear_row(v, r, 0, VT_COLS);
	vt_dirty_range(v, top, bot);
}

static void vt_scroll_down(struct vt *v, int n)
{
	if (n <= 0) return;
	int top = v->scroll_top, bot = v->scroll_bot;
	if (bot < top) return;
	int height = bot - top + 1;
	if (n > height) n = height;

	for (int r = bot; r - n >= top; r--) {
		for (int c = 0; c < VT_COLS; c++)
			v->cells[r][c] = v->cells[r - n][c];
	}
	for (int r = top; r < top + n; r++)
		vt_clear_row(v, r, 0, VT_COLS);
	vt_dirty_range(v, top, bot);
}

/* ---- cursor + plain-char placement ------------------------------------- */

static void vt_clamp_cursor(struct vt *v)
{
	if (v->row < 0)         v->row = 0;
	if (v->row >= VT_ROWS)  v->row = VT_ROWS - 1;
	if (v->col < 0)         v->col = 0;
	if (v->col >= VT_COLS)  v->col = VT_COLS - 1;
}

/* Advance one row, scrolling the region if we walked off the bottom.
 * Column is left alone (caller handles CR separately). */
static void vt_cursor_down(struct vt *v)
{
	if (v->row == v->scroll_bot)
		vt_scroll_up(v, 1);
	else if (v->row < VT_ROWS - 1)
		v->row++;
}

/* Reverse-index: opposite of cursor_down. */
static void vt_cursor_up_scroll(struct vt *v)
{
	if (v->row == v->scroll_top)
		vt_scroll_down(v, 1);
	else if (v->row > 0)
		v->row--;
}

static void vt_put_printable(struct vt *v, uint8_t ch)
{
	if (v->wrap_pending) {
		v->col = 0;
		vt_cursor_down(v);
		v->wrap_pending = 0;
	}
	v->cells[v->row][v->col].ch    = ch;
	v->cells[v->row][v->col].fg    = v->cur_fg;
	v->cells[v->row][v->col].bg    = v->cur_bg;
	v->cells[v->row][v->col].flags = v->cur_flags;
	vt_dirty_row(v, v->row);
	if (v->col == VT_COLS - 1) {
		if (v->autowrap)
			v->wrap_pending = 1;
		/* If autowrap is off, the cursor stays on the right
		 * margin and subsequent prints overwrite the same
		 * cell -- xterm/vt100 behaviour. */
	} else {
		v->col++;
	}
}

/* ---- control bytes ----------------------------------------------------- */

static void vt_ctl(struct vt *v, uint8_t b)
{
	switch (b) {
	case 0x07:	/* BEL */
		v->bell_pending = 1;
		break;
	case 0x08:	/* BS */
		v->wrap_pending = 0;
		if (v->col > 0) v->col--;
		break;
	case 0x09: {	/* HT -- next multiple of 8 */
		int next = (v->col & ~7) + 8;
		if (next >= VT_COLS) next = VT_COLS - 1;
		v->col = next;
		v->wrap_pending = 0;
		break;
	}
	case 0x0a:	/* LF */
	case 0x0b:	/* VT */
	case 0x0c:	/* FF */
		vt_cursor_down(v);
		v->wrap_pending = 0;
		break;
	case 0x0d:	/* CR */
		v->col = 0;
		v->wrap_pending = 0;
		break;
	default:
		/* All other C0 control bytes (NUL, SOH, ...) are
		 * discarded.  Real terminals also drop them in
		 * GROUND state. */
		break;
	}
}

/* ---- ESC (single-byte finals) ----------------------------------------- */

static void vt_reset_defaults(struct vt *v)
{
	v->cur_fg     = VT_COLOR_DEFAULT;
	v->cur_bg     = VT_COLOR_DEFAULT;
	v->cur_flags  = 0;
	v->scroll_top = 0;
	v->scroll_bot = VT_ROWS - 1;
	v->autowrap   = 1;
	v->cursor_visible = 1;
	v->wrap_pending   = 0;
}

static void vt_handle_esc(struct vt *v, uint8_t b)
{
	switch (b) {
	case 'c':	/* RIS -- reset to initial state */
		vt_reset_defaults(v);
		vt_clear_region(v, 0, VT_ROWS);
		v->row = 0;
		v->col = 0;
		break;
	case 'D':	/* IND -- index (cursor down + scroll) */
		vt_cursor_down(v);
		v->wrap_pending = 0;
		break;
	case 'M':	/* RI -- reverse index */
		vt_cursor_up_scroll(v);
		v->wrap_pending = 0;
		break;
	case 'E':	/* NEL -- next line (CR + LF) */
		v->col = 0;
		vt_cursor_down(v);
		v->wrap_pending = 0;
		break;
	case '7':	/* DECSC -- save cursor + attrs */
		v->saved_row   = v->row;
		v->saved_col   = v->col;
		v->saved_fg    = v->cur_fg;
		v->saved_bg    = v->cur_bg;
		v->saved_flags = v->cur_flags;
		break;
	case '8':	/* DECRC -- restore cursor + attrs */
		v->row        = v->saved_row;
		v->col        = v->saved_col;
		v->cur_fg     = v->saved_fg;
		v->cur_bg     = v->saved_bg;
		v->cur_flags  = v->saved_flags;
		vt_clamp_cursor(v);
		v->wrap_pending = 0;
		break;
	default:
		/* Ignore unknown ESC <byte> sequences -- real terminals
		 * silently drop them.  We don't want a hostile input
		 * stream to leave us in a bad state. */
		break;
	}
	v->state = VT_S_GROUND;
}

/* ---- CSI dispatch ----------------------------------------------------- */

/* params[i], defaulting to `def` if the param wasn't supplied or is 0. */
static int vt_p(const struct vt *v, int i, int def)
{
	if (i >= v->n_params)            return def;
	if (v->params[i] == 0)           return def;
	return v->params[i];
}

/* SGR (Select Graphic Rendition): walk params, apply each. */
static void vt_sgr(struct vt *v)
{
	if (v->n_params == 0) {
		/* "ESC [ m" alone means "ESC [ 0 m" = reset. */
		v->cur_fg    = VT_COLOR_DEFAULT;
		v->cur_bg    = VT_COLOR_DEFAULT;
		v->cur_flags = 0;
		return;
	}
	for (int i = 0; i < v->n_params; i++) {
		int p = v->params[i];
		if (p == 0) {
			v->cur_fg    = VT_COLOR_DEFAULT;
			v->cur_bg    = VT_COLOR_DEFAULT;
			v->cur_flags = 0;
		} else if (p == 1)        v->cur_flags |=  VT_FLAG_BOLD;
		else if (p == 4)          v->cur_flags |=  VT_FLAG_UNDERLINE;
		else if (p == 7)          v->cur_flags |=  VT_FLAG_REVERSE;
		else if (p == 22)         v->cur_flags &= ~VT_FLAG_BOLD;
		else if (p == 24)         v->cur_flags &= ~VT_FLAG_UNDERLINE;
		else if (p == 27)         v->cur_flags &= ~VT_FLAG_REVERSE;
		else if (p >= 30 && p <= 37)  v->cur_fg = (uint8_t)(p - 30);
		else if (p == 39)         v->cur_fg = VT_COLOR_DEFAULT;
		else if (p >= 40 && p <= 47)  v->cur_bg = (uint8_t)(p - 40);
		else if (p == 49)         v->cur_bg = VT_COLOR_DEFAULT;
		else if (p >= 90 && p <= 97)
			v->cur_fg = (uint8_t)((p - 90) | VT_COLOR_BRIGHT);
		else if (p >= 100 && p <= 107)
			v->cur_bg = (uint8_t)((p - 100) | VT_COLOR_BRIGHT);
		/* Unknown SGR codes silently ignored (real terminals do
		 * the same; the next param keeps the walk going). */
	}
}

/* Erase in display (J).
 *   0 (default): from cursor to end of screen.
 *   1          : from start of screen to cursor (inclusive).
 *   2          : whole screen.
 *   3          : whole screen + scrollback (we have none -- same as 2).
 */
static void vt_ed(struct vt *v)
{
	int mode = vt_p(v, 0, 0);
	if (mode == 0 || v->n_params == 0) {
		vt_clear_row(v, v->row, v->col, VT_COLS);
		vt_clear_region(v, v->row + 1, VT_ROWS);
	} else if (mode == 1) {
		vt_clear_region(v, 0, v->row);
		vt_clear_row(v, v->row, 0, v->col + 1);
	} else if (mode == 2 || mode == 3) {
		vt_clear_region(v, 0, VT_ROWS);
	}
}

/* Erase in line (K). */
static void vt_el(struct vt *v)
{
	int mode = vt_p(v, 0, 0);
	if (mode == 0 || v->n_params == 0)
		vt_clear_row(v, v->row, v->col, VT_COLS);
	else if (mode == 1)
		vt_clear_row(v, v->row, 0, v->col + 1);
	else if (mode == 2)
		vt_clear_row(v, v->row, 0, VT_COLS);
}

static void vt_decset_decrst(struct vt *v, int set)
{
	for (int i = 0; i < v->n_params; i++) {
		switch (v->params[i]) {
		case 7:		v->autowrap = set;       break;
		case 25:	v->cursor_visible = set; break;
		default: /* unknown DEC private mode -- ignore */ break;
		}
	}
}

static void vt_dispatch_csi(struct vt *v, uint8_t final)
{
	int n;
	switch (final) {
	case 'A':	/* CUU */
		n = vt_p(v, 0, 1);
		v->row -= n;
		v->wrap_pending = 0;
		break;
	case 'B':	/* CUD */
		n = vt_p(v, 0, 1);
		v->row += n;
		v->wrap_pending = 0;
		break;
	case 'C':	/* CUF */
		n = vt_p(v, 0, 1);
		v->col += n;
		v->wrap_pending = 0;
		break;
	case 'D':	/* CUB */
		n = vt_p(v, 0, 1);
		v->col -= n;
		v->wrap_pending = 0;
		break;
	case 'E':	/* CNL */
		n = vt_p(v, 0, 1);
		v->row += n;
		v->col = 0;
		v->wrap_pending = 0;
		break;
	case 'F':	/* CPL */
		n = vt_p(v, 0, 1);
		v->row -= n;
		v->col = 0;
		v->wrap_pending = 0;
		break;
	case 'G':	/* CHA -- 1-based column */
		v->col = vt_p(v, 0, 1) - 1;
		v->wrap_pending = 0;
		break;
	case 'H':	/* CUP */
	case 'f':	/* HVP -- alias of CUP */
		v->row = vt_p(v, 0, 1) - 1;
		v->col = vt_p(v, 1, 1) - 1;
		v->wrap_pending = 0;
		break;
	case 'J':	vt_ed(v); break;
	case 'K':	vt_el(v); break;
	case 'L':	/* IL -- insert N lines at cursor */
		n = vt_p(v, 0, 1);
		if (v->row >= v->scroll_top && v->row <= v->scroll_bot) {
			int save_top = v->scroll_top;
			v->scroll_top = v->row;
			vt_scroll_down(v, n);
			v->scroll_top = save_top;
		}
		break;
	case 'M':	/* DL -- delete N lines */
		n = vt_p(v, 0, 1);
		if (v->row >= v->scroll_top && v->row <= v->scroll_bot) {
			int save_top = v->scroll_top;
			v->scroll_top = v->row;
			vt_scroll_up(v, n);
			v->scroll_top = save_top;
		}
		break;
	case '@': {	/* ICH -- insert N blank chars at cursor */
		n = vt_p(v, 0, 1);
		if (n > VT_COLS - v->col) n = VT_COLS - v->col;
		for (int c = VT_COLS - 1; c >= v->col + n; c--)
			v->cells[v->row][c] = v->cells[v->row][c - n];
		for (int c = v->col; c < v->col + n; c++) {
			v->cells[v->row][c].ch    = ' ';
			v->cells[v->row][c].fg    = v->cur_fg;
			v->cells[v->row][c].bg    = v->cur_bg;
			v->cells[v->row][c].flags = 0;
		}
		vt_dirty_row(v, v->row);
		break;
	}
	case 'P': {	/* DCH -- delete N chars at cursor */
		n = vt_p(v, 0, 1);
		if (n > VT_COLS - v->col) n = VT_COLS - v->col;
		for (int c = v->col; c < VT_COLS - n; c++)
			v->cells[v->row][c] = v->cells[v->row][c + n];
		for (int c = VT_COLS - n; c < VT_COLS; c++) {
			v->cells[v->row][c].ch    = ' ';
			v->cells[v->row][c].fg    = v->cur_fg;
			v->cells[v->row][c].bg    = v->cur_bg;
			v->cells[v->row][c].flags = 0;
		}
		vt_dirty_row(v, v->row);
		break;
	}
	case 'S':	vt_scroll_up(v, vt_p(v, 0, 1)); break;
	case 'T':	vt_scroll_down(v, vt_p(v, 0, 1)); break;
	case 'm':	vt_sgr(v); break;
	case 'r': {	/* DECSTBM -- set scroll region */
		int top = vt_p(v, 0, 1) - 1;
		int bot = vt_p(v, 1, VT_ROWS) - 1;
		if (top < 0) top = 0;
		if (bot >= VT_ROWS) bot = VT_ROWS - 1;
		if (top < bot) {
			v->scroll_top = top;
			v->scroll_bot = bot;
			/* DECSTBM also moves the cursor home. */
			v->row = 0;
			v->col = 0;
		}
		break;
	}
	case 's':	/* SCP -- ANSI save cursor */
		v->saved_row   = v->row;
		v->saved_col   = v->col;
		v->saved_fg    = v->cur_fg;
		v->saved_bg    = v->cur_bg;
		v->saved_flags = v->cur_flags;
		break;
	case 'u':	/* RCP -- ANSI restore cursor */
		v->row        = v->saved_row;
		v->col        = v->saved_col;
		v->cur_fg     = v->saved_fg;
		v->cur_bg     = v->saved_bg;
		v->cur_flags  = v->saved_flags;
		v->wrap_pending = 0;
		break;
	case 'h':	/* SM / DECSET */
		if (v->csi_private) vt_decset_decrst(v, 1);
		/* ANSI SM (no '?') -- IRM and a few others we don't
		 * model yet.  Drop. */
		break;
	case 'l':	/* RM / DECRST */
		if (v->csi_private) vt_decset_decrst(v, 0);
		break;
	default:
		/* Unknown CSI final -- silently drop, same as
		 * real terminals. */
		break;
	}
	vt_clamp_cursor(v);
	v->state = VT_S_GROUND;
}

/* ---- public API ------------------------------------------------------- */

void vt_init(struct vt *v)
{
	for (int r = 0; r < VT_ROWS; r++)
		for (int c = 0; c < VT_COLS; c++) {
			v->cells[r][c].ch    = ' ';
			v->cells[r][c].fg    = VT_COLOR_DEFAULT;
			v->cells[r][c].bg    = VT_COLOR_DEFAULT;
			v->cells[r][c].flags = 0;
		}
	v->row = v->col = 0;
	v->saved_row = v->saved_col = 0;
	v->cur_fg = v->saved_fg = VT_COLOR_DEFAULT;
	v->cur_bg = v->saved_bg = VT_COLOR_DEFAULT;
	v->cur_flags = v->saved_flags = 0;
	v->scroll_top = 0;
	v->scroll_bot = VT_ROWS - 1;
	v->autowrap = 1;
	v->cursor_visible = 1;
	v->wrap_pending = 0;
	v->state = VT_S_GROUND;
	v->n_params = 0;
	v->csi_private = 0;
	v->bell_pending = 0;
	v->dirty_top = VT_ROWS;	/* > dirty_bot means "nothing dirty" */
	v->dirty_bot = -1;
	for (int i = 0; i < VT_PARAMS_MAX; i++) v->params[i] = 0;
}

void vt_feed(struct vt *v, uint8_t b)
{
	switch (v->state) {
	case VT_S_GROUND:
		if (b == 0x1b) {
			v->state = VT_S_ESC;
		} else if (b < 0x20) {
			vt_ctl(v, b);
		} else if (b < 0x7f) {
			vt_put_printable(v, b);
		} else if (b == 0x7f) {
			/* DEL -- terminals normally ignore. */
		} else {
			/* High bit set: 8-bit / UTF-8 byte.  For
			 * Phase 1 we store it raw so a renderer that
			 * understands UTF-8 can recover; phase later
			 * will buffer multi-byte sequences. */
			vt_put_printable(v, b);
		}
		break;
	case VT_S_ESC:
		if (b == '[') {
			v->state = VT_S_CSI;
			v->n_params = 0;
			v->params[0] = 0;
			v->csi_private = 0;
		} else {
			vt_handle_esc(v, b);
		}
		break;
	case VT_S_CSI:
		if (b == '?' && v->n_params == 0) {
			v->csi_private = 1;
		} else if (b >= '0' && b <= '9') {
			if (v->n_params == 0) v->n_params = 1;
			int idx = v->n_params - 1;
			v->params[idx] = v->params[idx] * 10 + (b - '0');
		} else if (b == ';') {
			if (v->n_params < VT_PARAMS_MAX) {
				v->n_params++;
				v->params[v->n_params - 1] = 0;
			}
		} else if (b >= 0x40 && b <= 0x7e) {
			/* Final byte -- dispatch. */
			vt_dispatch_csi(v, b);
		}
		/* Other intermediate / unknown bytes are dropped; the
		 * sequence either completes with a final or is reset
		 * when a stray ESC arrives. */
		break;
	}
}

void vt_feed_bytes(struct vt *v, const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		vt_feed(v, buf[i]);
}

int vt_take_bell(struct vt *v)
{
	int b = v->bell_pending;
	v->bell_pending = 0;
	return b;
}

int vt_take_dirty(struct vt *v, int *top, int *bot)
{
	if (v->dirty_top > v->dirty_bot) return 0;
	*top = v->dirty_top;
	*bot = v->dirty_bot;
	v->dirty_top = VT_ROWS;
	v->dirty_bot = -1;
	return 1;
}

void vt_mark_all_dirty(struct vt *v)
{
	v->dirty_top = 0;
	v->dirty_bot = VT_ROWS - 1;
}

const struct vt_cell *vt_cell_at(const struct vt *v, int row, int col)
{
	if (row < 0 || row >= VT_ROWS) return 0;
	if (col < 0 || col >= VT_COLS) return 0;
	return &v->cells[row][col];
}

/* ---- self-test -------------------------------------------------------- */

/* Static buffer -- 1.6 KiB stack frame is over our 4 KiB kthread
 * stack budget if we ever ran this from a thread.  At kmain time
 * the stack is plenty, but keeping it static documents the intent. */
static struct vt vt_selftest_buf;

static int vt_st_expect_ch(const struct vt *v, int r, int c, char want,
			   const char *what)
{
	const struct vt_cell *cell = vt_cell_at(v, r, c);
	if (!cell || cell->ch != (uint8_t)want) {
		kprintf("vt: SELFTEST FAIL %s: r=%d c=%d got=0x%x want=0x%x\n",
			what, r, c,
			cell ? cell->ch : 0, (unsigned)(uint8_t)want);
		return 0;
	}
	return 1;
}

static int vt_st_expect_attr(const struct vt *v, int r, int c,
			     uint8_t fg, uint8_t bg, uint8_t flags,
			     const char *what)
{
	const struct vt_cell *cell = vt_cell_at(v, r, c);
	if (!cell || cell->fg != fg || cell->bg != bg
	          || cell->flags != flags) {
		kprintf("vt: SELFTEST FAIL %s: r=%d c=%d got fg=%u bg=%u fl=0x%x\n",
			what, r, c, cell ? cell->fg : 0,
			cell ? cell->bg : 0, cell ? cell->flags : 0);
		return 0;
	}
	return 1;
}

void vt_selftest(void)
{
	struct vt *v = &vt_selftest_buf;
	int ok = 1;

	/* (1) Plain text on row 0, "Hi". */
	vt_init(v);
	vt_feed(v, 'H');
	vt_feed(v, 'i');
	ok &= vt_st_expect_ch(v, 0, 0, 'H', "plain.H");
	ok &= vt_st_expect_ch(v, 0, 1, 'i', "plain.i");
	if (v->row != 0 || v->col != 2) {
		kprintf("vt: SELFTEST FAIL plain.cursor: r=%d c=%d\n",
			v->row, v->col);
		ok = 0;
	}

	/* (2) CR + LF moves to next row col 0. */
	vt_feed(v, '\r');
	vt_feed(v, '\n');
	if (v->row != 1 || v->col != 0) {
		kprintf("vt: SELFTEST FAIL crlf: r=%d c=%d\n",
			v->row, v->col);
		ok = 0;
	}

	/* (3) SGR red foreground + bold + 'R'. */
	const uint8_t sgr[] = "\x1b[1;31mR";
	vt_feed_bytes(v, sgr, sizeof(sgr) - 1);
	ok &= vt_st_expect_ch  (v, 1, 0, 'R', "sgr.ch");
	ok &= vt_st_expect_attr(v, 1, 0, VT_COLOR_RED, VT_COLOR_DEFAULT,
	                       VT_FLAG_BOLD, "sgr.attr");

	/* (4) CSI H positions cursor at (1,1) which is row 0 col 0. */
	const uint8_t cup[] = "\x1b[1;1HX";
	vt_feed_bytes(v, cup, sizeof(cup) - 1);
	ok &= vt_st_expect_ch(v, 0, 0, 'X', "cup.ch");

	/* (5) CSI 2J clears whole screen; previous 'X' goes away. */
	const uint8_t ed[] = "\x1b[2J\x1b[1;1H";
	vt_feed_bytes(v, ed, sizeof(ed) - 1);
	ok &= vt_st_expect_ch(v, 0, 0, ' ', "ed.cleared");

	/* (6) Autowrap: feed 81 'a's onto row 0; the 81st lands at
	 *     row 1 col 0 thanks to xterm phantom-cursor wrap. */
	vt_init(v);
	for (int i = 0; i < 80; i++) vt_feed(v, 'a');
	vt_feed(v, 'b');
	ok &= vt_st_expect_ch(v, 0, 79, 'a', "wrap.last");
	ok &= vt_st_expect_ch(v, 1, 0,  'b', "wrap.next");

	/* (7) Scroll: write LF at last row, top row should vanish. */
	vt_init(v);
	vt_feed(v, 'T');
	for (int i = 0; i < VT_ROWS; i++)
		vt_feed(v, '\n');
	ok &= vt_st_expect_ch(v, 0, 0, ' ', "scroll.gone");

	if (ok)
		kprintf("vt: selftest PASS\n");
}

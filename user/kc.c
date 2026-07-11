/*
 * user/kc.c -- Norton Commander-style two-panel file manager
 *
 * This file is textually #included into user/init.c so it reuses
 * the shell's helpers (cwrite, cputc, vt_move, resolve_path, cwd,
 * ustrlen, ustrcmp, udec).  Living inside the init binary means no
 * second user blob is needed -- the shell's "kc" verb just calls
 * kc_main(), which paints the panels, runs an input loop, and
 * returns to the prompt on F10/Esc/q.
 *
 * Layout (80x24, hardcoded -- NC was 80x25 / 80x24 forever)
 * --------------------------------------------------------
 *   Row 1            Header bar.  Two paths: left + right.
 *   Rows 2..21       Panel rows (20 entries each).
 *   Row 22           Horizontal separator.
 *   Row 23           Info bar: selected entry's full path + size.
 *   Row 24           Function-key footer (F1..F10).
 *
 * Colors -- classic Norton Commander palette
 * ------------------------------------------
 *   Panel cells      light grey on blue          (normal)
 *   Directories      bold bright-white on blue
 *   Selected cell    black on cyan               (NC inverse)
 *   Header bar       black on cyan
 *   Active header    bold black on white         (which panel has focus)
 *   Footer keys      black on cyan, number white on black
 *   Status bar       black on cyan
 *
 * Function keys
 * -------------
 *   F1 Help    F2 Menu*   F3 View    F4 Edit    F5 Copy
 *   F6 RenMov  F7 Mkdir   F8 Delete  F9 Menu*   F10 Quit
 *
 *   * Stubs (no pull-down menu system yet).
 *
 *   Number keys 1..0 also fire F1..F10 (NC keymap).
 *   q / Esc also quit.
 *   Tab switches active panel.
 *   Enter descends into a directory or views a regular file.
 *
 * State / sizes
 * -------------
 *   KC_MAX_ENT 128       Per-panel entry cap.  /proc + / + /dev together
 *                        are nowhere near that; if a real dir overflows
 *                        we silently truncate.
 *   raw[4096]            Static scratch for sys_ll output (shared).
 *
 * No malloc.  All buffers are file-scope or stack.
 */

#define KC_W           80
#define KC_H           24

/*
 * Layout, top-to-bottom:
 *   Row 1            Top border ╔═══<lpath>═══╦═══ Info ═══╗
 *   Rows 2..21       Panel rows (20 entries each); left = listing,
 *                    right = info pane (folder props or file preview).
 *                    Each row is bordered: ║ ... ║ ... ║.
 *   Row 22           Bottom border ╚═══╩═══╝
 *   Row 23           Status: selected entry's full path + size.
 *   Row 24           Function-key footer (F1..F10).
 *
 * Columns:
 *   Col 1            Left border ║
 *   Cols 2..39       Left panel content (38 cells)
 *   Col 40           Middle separator ║
 *   Cols 41..79      Right panel (info) content (39 cells)
 *   Col 80           Right border ║
 */
#define KC_TOP_ROW     1
#define KC_LIST_TOP    2
#define KC_LIST_BOT    21
#define KC_LIST_ROWS   (KC_LIST_BOT - KC_LIST_TOP + 1)   /* 20 */
#define KC_BOT_ROW     22
#define KC_INFO_ROW    23
#define KC_FN_ROW      24

#define KC_BORDER_L_COL    1
#define KC_LEFT_COL        2
#define KC_BORDER_M_COL    40
#define KC_RIGHT_COL       41
#define KC_BORDER_R_COL    80
#define KC_LEFT_W          38
#define KC_RIGHT_W         39

/* Compatibility aliases for the old left-side-only render-cell path.
 * kc_render_cell still draws a 38-cell row starting at base_col. */
#define KC_MID_COL         KC_RIGHT_COL
#define KC_PANEL_W         KC_LEFT_W

/* Double-line box drawing -- CP437 lineage, UTF-8 encoded.  Each
 * glyph is 3 bytes in the output stream but occupies 1 terminal
 * cell, so cursor-column math counts each cwrite() of one of these
 * as one column advance. */
#define KC_BOX_TL  "\xe2\x95\x94"  /* ╔ */
#define KC_BOX_TR  "\xe2\x95\x97"  /* ╗ */
#define KC_BOX_BL  "\xe2\x95\x9a"  /* ╚ */
#define KC_BOX_BR  "\xe2\x95\x9d"  /* ╝ */
#define KC_BOX_TD  "\xe2\x95\xa6"  /* ╦ */
#define KC_BOX_BU  "\xe2\x95\xa9"  /* ╩ */
#define KC_BOX_H   "\xe2\x95\x90"  /* ═ */
#define KC_BOX_V   "\xe2\x95\x91"  /* ║ */

#define KC_MAX_ENT     128
#define KC_NAME_MAX    32

struct kc_ent {
	char name[KC_NAME_MAX];
	char type;	/* 'd' dir, 'r' regular, 'c' chrdev, '?' unknown */
	long size;
};

struct kc_panel {
	char          path[128];
	struct kc_ent ents[KC_MAX_ENT];
	int           n;
	int           cur;
	int           top;
};

/* Forward decls to break the kc_op_enter -> kc_op_view cycle and
 * to keep all symbols static. */
static void kc_op_view(void);
static void kc_render_all(void);

static struct kc_panel kc_panels[2];
static int             kc_active;	/* 0 = left, 1 = right */
static int             kc_quit;
static char            kc_scratch[4096];

/*
 * Output buffering for the painting paths.
 *
 * init.c's cwrite/cputc go straight to sys_write, one syscall per
 * call.  A full repaint emits 5k+ bytes (24 rows * ~80 cells * a few
 * escape bytes), which used to be 5k+ syscalls and seconds of paint
 * time on QEMU.  We funnel every cwrite/cputc through kc_obuf for
 * the duration of this file and flush once per logical update.
 *
 * Macro shim approach: the kc rendering code keeps calling cwrite/
 * cputc as before; the macros below redirect to the buffer; #undef'd
 * at the end of kc.c so the rest of init.c (dispatch, _start) still
 * sees the unbuffered helpers.
 */
static char kc_obuf[4096];
static int  kc_olen;

/*
 * The kernel's stream_putmsg allocb's an mblk of write_len bytes, and
 * kmalloc tops out at the size-2048 slab cache.  Anything bigger
 * fails with "kmalloc(N): too large" and the bytes are dropped.
 * Match the existing KLOG_CHUNK convention and flush in 1KB pieces.
 */
#define KC_FLUSH_CHUNK	1024

static void kc_flush(void)
{
	int off = 0;
	while (off < kc_olen) {
		int n = kc_olen - off;
		if (n > KC_FLUSH_CHUNK) n = KC_FLUSH_CHUNK;
		sys_write(fd_console, kc_obuf + off, (size_t)n);
		off += n;
	}
	kc_olen = 0;
}

static void kc_buf_w(const char *s, int n)
{
	if (kc_olen + n > (int)sizeof(kc_obuf)) kc_flush();
	if (n > (int)sizeof(kc_obuf)) {
		/* Doesn't fit even after flush -- write in chunks directly. */
		int off = 0;
		while (off < n) {
			int w = n - off;
			if (w > KC_FLUSH_CHUNK) w = KC_FLUSH_CHUNK;
			sys_write(fd_console, s + off, (size_t)w);
			off += w;
		}
		return;
	}
	for (int i = 0; i < n; i++) kc_obuf[kc_olen++] = s[i];
}

static void kc_buf_ws(const char *s)
{
	int n = 0;
	while (s[n]) n++;
	kc_buf_w(s, n);
}

static void kc_buf_wc(char c)
{
	if (kc_olen >= (int)sizeof(kc_obuf)) kc_flush();
	kc_obuf[kc_olen++] = c;
}

#define cwrite(s)  kc_buf_ws(s)
#define cputc(c)   kc_buf_wc(c)

/* ---- attribute primitives ---------------------------------------------- */
/*
 * Sequences chosen for portability: any VT100/xterm/Linux-console
 * descendant reads CSI <fg>;<bg>m.  We never assume true-color; only
 * the 16-color base palette.
 */
static void kc_attr_reset(void)    { cwrite("\033[0m"); }
static void kc_attr_normal(void)   { cwrite("\033[0;37;44m"); }   /* light grey on blue (classic NC) */
static void kc_attr_select(void)   { cwrite("\033[0;30;46m"); }   /* black on cyan -- selected entry */
static void kc_attr_dir(void)      { cwrite("\033[1;97;44m"); }   /* bold bright-white on blue -- dirs */
static void kc_attr_dir_sel(void)  { cwrite("\033[1;30;46m"); }   /* bold black on cyan -- selected dir */
/* header/header_act are dormant now that the top border embeds the
 * title in the normal blue palette.  Keep them around for the eventual
 * Tab-toggles-active-panel work without dragging unused-fn errors. */
__attribute__((unused))
static void kc_attr_header(void)   { cwrite("\033[0;30;46m"); }   /* black on cyan */
__attribute__((unused))
static void kc_attr_header_act(void){ cwrite("\033[1;30;47m"); }  /* bold black on white -- active panel */
static void kc_attr_footer(void)   { cwrite("\033[0;30;46m"); }
static void kc_attr_fkey_num(void) { cwrite("\033[1;37;40m"); }   /* white-on-black: the F-key number */
static void kc_attr_status(void)   { cwrite("\033[0;30;46m"); }

static void kc_clear_screen(void)  { cwrite("\033[2J\033[H"); }
static void kc_hide_cursor(void)   { cwrite("\033[?25l"); }
static void kc_show_cursor(void)   { cwrite("\033[?25h"); }

/* Self-contained vt_move using our (macro'd) cwrite/cputc so the
 * cursor-position escapes also flow through the kc output buffer.
 * If we delegated to init.c's vt_move it would call the unbuffered
 * cwrite directly and defeat the batching. */
static void kc_move(int r, int c)
{
	char buf[16];
	cwrite("\033[");
	udec(buf, r); cwrite(buf);
	cputc(';');
	udec(buf, c); cwrite(buf);
	cputc('H');
}

/* ---- string utilities -------------------------------------------------- */

static void kc_copy_str(char *dst, const char *src, size_t cap)
{
	size_t i = 0;
	for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
	dst[i] = '\0';
}

static void kc_pad_spaces(int n)
{
	for (int i = 0; i < n; i++) cputc(' ');
}

/* Render an integer right-aligned into a fixed-width field. */
static void kc_print_size(long v, int width)
{
	char  buf[24];
	udec(buf, v);
	int len = (int)ustrlen(buf);
	if (width <= 0 || len >= width) {
		/* No padding requested, or number is already wider than
		 * the field -- print as-is.  We don't bother truncating
		 * the high end; long-long byte counts are rare here. */
		cwrite(buf);
		return;
	}
	kc_pad_spaces(width - len);
	cwrite(buf);
}

/* ---- directory loader -------------------------------------------------- */

/*
 * Parse one line of sys_ll output: "<3-tag> <8-size> <name>\n".
 *   tag  : "dir" | "reg" | "chr" (3 chars exactly)
 *   size : 8-char field, right-aligned decimal, or "MMMM,NNN" for chrdev
 *   name : up to next \n
 * Returns next offset, or -1 on malformed.
 */
static int kc_parse_line(const char *buf, int off, int cap, struct kc_ent *e)
{
	if (off >= cap) return -1;

	/* type tag is the first char of the line */
	char t = buf[off];
	e->type = (t == 'd') ? 'd'
	        : (t == 'c') ? 'c'
	        : (t == 'b') ? 'b'
	        : (t == 'r') ? 'r'
	        : '?';

	/* Find end-of-line and the last space before it.  The format is
	 *     "tag <8-char-size-field> <name>\n"
	 * where the size field can contain internal whitespace (chrdev's
	 * "MMMM,NNN" is right-justified within 8 chars and starts with
	 * padding spaces).  We don't want to commit to a fixed offset,
	 * so we just split at the LAST space before \n -- everything to
	 * the right of it is the name. */
	int eol = off;
	while (eol < cap && buf[eol] != '\n') eol++;
	int last_sp = -1;
	for (int k = off; k < eol; k++) if (buf[k] == ' ') last_sp = k;
	if (last_sp < 0) {
		/* malformed line */
		e->name[0] = '\0';
		e->size = 0;
		return (eol < cap) ? eol + 1 : eol;
	}

	/* Parse size: digits-only in the metadata.  Device rows carry
	 * "MMMM,NNN" (major,minor) instead of a byte count; keep both
	 * halves, encoded (major << 16) | minor, so the renderers can
	 * show the real dev_t instead of a bogus "0". */
	long sz = 0, maj = 0;
	int  has_comma = 0;
	for (int k = off + 1; k < last_sp; k++) {
		char c = buf[k];
		if (c >= '0' && c <= '9') sz = sz * 10 + (c - '0');
		else if (c == ',')        { has_comma = 1; maj = sz; sz = 0; }
	}
	e->size = has_comma ? ((maj << 16) | (sz & 0xffff)) : sz;

	/* Copy name = buf[last_sp+1 .. eol). */
	int i = 0;
	for (int k = last_sp + 1; k < eol && i + 1 < KC_NAME_MAX; k++) {
		e->name[i++] = buf[k];
	}
	e->name[i] = '\0';
	return (eol < cap) ? eol + 1 : eol;
}

static void kc_panel_load(struct kc_panel *p)
{
	p->n   = 0;
	p->cur = 0;
	p->top = 0;

	/* Synthetic ".." -- only when not at root */
	if (!(p->path[0] == '/' && p->path[1] == '\0')) {
		struct kc_ent *e = &p->ents[p->n++];
		e->name[0] = '.'; e->name[1] = '.'; e->name[2] = '\0';
		e->type = 'd';
		e->size = 0;
	}

	long n = sys_ll(p->path, kc_scratch, sizeof(kc_scratch));
	if (n < 0) return;

	int off = 0;
	while (off < (int)n && p->n < KC_MAX_ENT) {
		struct kc_ent tmp;
		int next = kc_parse_line(kc_scratch, off, (int)n, &tmp);
		if (next <= off) break;
		if (tmp.name[0] != '\0' && tmp.name[0] != '\n') {
			p->ents[p->n++] = tmp;
		}
		off = next;
	}
}

/* ---- rendering --------------------------------------------------------- */

/*
 * Render one left-panel cell.  Layout within KC_LEFT_W = 38 columns:
 *
 *   col 0       leading space
 *   col 1..26   name (truncated, padded -- 26 cells)
 *   col 27..37  size (right-aligned 11 chars: 1 leading sep space +
 *               10-char value) -- "DIR" / "CHAR" / decimal bytes
 *
 * Right panel is the info pane; nothing else calls this with
 * panel_idx==1 since kc_render_panel(1) was replaced by
 * kc_render_info.
 */
static void kc_render_cell(int panel_idx, int row, int row_in_panel)
{
	struct kc_panel *p = &kc_panels[panel_idx];
	int idx = p->top + row_in_panel;
	int base_col = (panel_idx == 0) ? KC_LEFT_COL : KC_RIGHT_COL;

	int selected = (idx == p->cur) && (panel_idx == kc_active);
	struct kc_ent *e = (idx < p->n) ? &p->ents[idx] : ((struct kc_ent *)0);

	if (e && e->type == 'd') {
		if (selected) kc_attr_dir_sel();
		else          kc_attr_dir();
	} else {
		if (selected) kc_attr_select();
		else          kc_attr_normal();
	}

	kc_move(row, base_col);
	cputc(' ');

	if (!e) {
		kc_pad_spaces(KC_LEFT_W - 1);
		return;
	}

	/* name: 26 cells */
	int name_cells = 26;
	int written = 0;
	for (int i = 0; e->name[i] && written < name_cells; i++) {
		cputc(e->name[i]);
		written++;
	}
	kc_pad_spaces(name_cells - written);

	/* size column: 11 cells right-aligned (1 sep space + 10 value) */
	if (e->type == 'd') {
		/* "DIR" right-aligned in 11 cells */
		kc_pad_spaces(8);
		cwrite("DIR");
	} else if (e->type == 'c' || e->type == 'b') {
		/* device numbers "M,N" right-aligned in 11 cells */
		char dv[16];
		long maj = (e->size >> 16) & 0xffff, min = e->size & 0xffff;
		int  n = 0;
		{ char t[8]; int i = 0;
		  if (maj == 0) t[i++] = '0';
		  while (maj) { t[i++] = (char)('0' + maj % 10); maj /= 10; }
		  while (i--) dv[n++] = t[i]; }
		dv[n++] = ',';
		{ char t[8]; int i = 0;
		  if (min == 0) t[i++] = '0';
		  while (min) { t[i++] = (char)('0' + min % 10); min /= 10; }
		  while (i--) dv[n++] = t[i]; }
		dv[n] = '\0';
		kc_pad_spaces(11 - n);
		cwrite(dv);
	} else {
		cputc(' ');
		kc_print_size(e->size, 10);
	}
}

static void kc_render_panel(int idx)
{
	for (int r = 0; r < KC_LIST_ROWS; r++) {
		kc_render_cell(idx, KC_LIST_TOP + r, r);
	}
}

/*
 * Right-panel "Info" view -- replaces the second listing.  Shows the
 * properties of the entry currently highlighted in the LEFT panel
 * (Norton Commander's Quickview / Info layout):
 *
 *   - For a directory: name, full path, total entries, cumulative
 *     byte count, and a Kappara-flavoured volume blurb (mem stats
 *     from /proc/meminfo if openable).
 *
 *   - For a regular file: name, full path, size, then up to ~14 head
 *     lines of the file's content, character-clipped at the right
 *     edge.  Binary data gets ?'s for non-printable bytes so the
 *     panel never decorates with stray escape sequences.
 *
 *   - For a character device or unknown type: just name + type tag,
 *     no read attempted.
 */
static char kc_info_buf[2048];

static void kc_info_print_line(int row, const char *s)
{
	kc_attr_normal();
	kc_move(row, KC_RIGHT_COL);
	cputc(' ');
	int written = 1;
	for (int i = 0; s[i] && written < KC_RIGHT_W - 1; i++) {
		char c = s[i];
		/* Treat <0x20 (except space wouldn't be here anyway) and
		 * 0x7F as ?.  >=0x80 is allowed so UTF-8 paths come
		 * through; the host terminal renders them. */
		if ((unsigned char)c < 0x20 || c == 0x7f) c = '?';
		cputc(c);
		written++;
	}
	kc_pad_spaces(KC_RIGHT_W - written);
}

static void kc_info_blank(int row)
{
	kc_attr_normal();
	kc_move(row, KC_RIGHT_COL);
	kc_pad_spaces(KC_RIGHT_W);
}

/* Compose a "Name: <name>" / "Path: <path>" / "Size: <n> bytes" line
 * triple into kc_info_buf at offset *off.  Returns the (offset, count)
 * structure indirectly via the buffer; caller picks lines off with
 * kc_info_split_lines. */
static int kc_info_emit_str(int off, const char *s)
{
	int i = 0;
	while (s[i] && off < (int)sizeof(kc_info_buf) - 1)
		kc_info_buf[off++] = s[i++];
	return off;
}

static int kc_info_emit_dec(int off, long v)
{
	char tmp[24];
	udec(tmp, v);
	return kc_info_emit_str(off, tmp);
}

static int kc_info_emit_nl(int off)
{
	if (off < (int)sizeof(kc_info_buf) - 1)
		kc_info_buf[off++] = '\n';
	return off;
}

/* Sniff for "binary or text" on the first N bytes.  Any NUL, any
 * 0x7F, or any control byte other than HT/LF/CR makes it binary.
 * Same rule POSIX `file(1)` uses internally before deciding whether
 * to print the file or run a magic match. */
static int kc_is_binary(const unsigned char *data, int len)
{
	int n = len > 256 ? 256 : len;
	for (int i = 0; i < n; i++) {
		unsigned char c = data[i];
		if (c == 0)                        return 1;
		if (c == 0x7f)                     return 1;
		if (c < 0x20 && c != '\t'
		             && c != '\n'
		             && c != '\r')         return 1;
	}
	return 0;
}

static const char kc_hex_digits[] = "0123456789abcdef";

static int kc_info_emit_hex_byte(int off, unsigned char b)
{
	if (off < (int)sizeof(kc_info_buf) - 1)
		kc_info_buf[off++] = kc_hex_digits[b >> 4];
	if (off < (int)sizeof(kc_info_buf) - 1)
		kc_info_buf[off++] = kc_hex_digits[b & 0xf];
	return off;
}

static int kc_info_emit_ch(int off, char c)
{
	if (off < (int)sizeof(kc_info_buf) - 1)
		kc_info_buf[off++] = c;
	return off;
}

/* Hex dump rows, classic xxd shape but compacted to fit the 38-cell
 * panel width (kc_info_print_line takes 1 leading space, so we have
 * 37 char-cells of content).
 *
 *     000  7f 45 4c 46 02 01 01 00 .ELF....
 *     ^^^  ^^^^^^^^^^^^^^^^^^^^^^^ ^^^^^^^^
 *      3            23 (8b + 7sp)     8
 *      + 2 spaces + 1 space = 37
 *
 * Limit shown rows to fit the ~14-line content slot under the file's
 * Name / Path / Size / "-- hex ----" header. */
static int kc_info_emit_hex_dump(int off, const unsigned char *data, int len)
{
	int max = len > 112 ? 112 : len;
	for (int i = 0; i < max; i += 8) {
		off = kc_info_emit_ch(off, kc_hex_digits[(i >> 8) & 0xf]);
		off = kc_info_emit_ch(off, kc_hex_digits[(i >> 4) & 0xf]);
		off = kc_info_emit_ch(off, kc_hex_digits[(i     ) & 0xf]);
		off = kc_info_emit_ch(off, ' ');
		off = kc_info_emit_ch(off, ' ');
		for (int j = 0; j < 8; j++) {
			if (i + j < len) {
				off = kc_info_emit_hex_byte(off, data[i + j]);
			} else {
				off = kc_info_emit_ch(off, ' ');
				off = kc_info_emit_ch(off, ' ');
			}
			if (j < 7) off = kc_info_emit_ch(off, ' ');
		}
		off = kc_info_emit_ch(off, ' ');
		for (int j = 0; j < 8 && i + j < len; j++) {
			unsigned char c = data[i + j];
			if (c < 0x20 || c >= 0x7f) c = '.';
			off = kc_info_emit_ch(off, (char)c);
		}
		off = kc_info_emit_nl(off);
	}
	return off;
}

static void kc_render_info(void)
{
	/* Compose the info text in kc_info_buf, line-separated by '\n'.
	 * Then walk lines through kc_info_print_line so each one gets
	 * clipped at the right edge and the row gets padded to the
	 * panel width. */
	struct kc_panel *p = &kc_panels[0];
	struct kc_ent  *e  = (p->cur < p->n) ? &p->ents[p->cur] : 0;

	int off = 0;
	off = kc_info_emit_str(off, "  Kappara Commander -- Info\n");
	off = kc_info_emit_nl(off);

	if (!e) {
		off = kc_info_emit_str(off, "  (empty selection)\n");
	} else {
		off = kc_info_emit_str(off, "  Name: ");
		off = kc_info_emit_str(off, e->name);
		off = kc_info_emit_nl(off);

		off = kc_info_emit_str(off, "  Path: ");
		off = kc_info_emit_str(off, p->path);
		if (!(p->path[0] == '/' && p->path[1] == '\0'))
			off = kc_info_emit_str(off, "/");
		off = kc_info_emit_str(off, e->name);
		off = kc_info_emit_nl(off);

		off = kc_info_emit_nl(off);

		if (e->type == 'd') {
			off = kc_info_emit_str(off, "  Type: directory\n");
			/* Per-directory totals are loaded into the OTHER
			 * panel only when the user descends.  Without a
			 * cheap "stat dir" syscall we just show the
			 * current listing's high-level counts. */
			off = kc_info_emit_str(off, "  ");
			off = kc_info_emit_dec(off, (long)p->n);
			off = kc_info_emit_str(off, " entries in parent\n");
		} else if (e->type == 'c') {
			off = kc_info_emit_str(off, "  Type: character device\n");
			off = kc_info_emit_str(off, "  Dev:  ");
			off = kc_info_emit_dec(off, (e->size >> 16) & 0xffff);
			off = kc_info_emit_str(off, ",");
			off = kc_info_emit_dec(off, e->size & 0xffff);
			off = kc_info_emit_nl(off);
		} else if (e->type == 'b') {
			off = kc_info_emit_str(off, "  Type: block device\n");
			off = kc_info_emit_str(off, "  Dev:  ");
			off = kc_info_emit_dec(off, (e->size >> 16) & 0xffff);
			off = kc_info_emit_str(off, ",");
			off = kc_info_emit_dec(off, e->size & 0xffff);
			off = kc_info_emit_nl(off);
		} else if (e->type == 'r') {
			off = kc_info_emit_str(off, "  Type: regular file\n");
			off = kc_info_emit_str(off, "  Size: ");
			off = kc_info_emit_dec(off, e->size);
			off = kc_info_emit_str(off, " bytes\n");

			/* Build the absolute path on the stack -- kc_scratch
			 * is the read buffer below. */
			char fpath[256];
			int pi = 0;
			for (int i = 0; p->path[i] && pi < (int)sizeof(fpath) - 2; i++)
				fpath[pi++] = p->path[i];
			if (!(p->path[0] == '/' && p->path[1] == '\0'))
				fpath[pi++] = '/';
			for (int i = 0; e->name[i] && pi < (int)sizeof(fpath) - 1; i++)
				fpath[pi++] = e->name[i];
			fpath[pi] = '\0';

			/* Read up to kc_scratch's capacity, then sniff and
			 * dispatch.  Binary -> hex dump; text -> head
			 * preview (line-walked through kc_info_print_line
			 * exactly as the existing branch did). */
			long fd = sys_open(fpath, 0);
			long n  = -1;
			if (fd >= 0) {
				n = sys_read(fd, kc_scratch,
				             (size_t)sizeof(kc_scratch));
				sys_close(fd);
			}
			if (n > 0) {
				off = kc_info_emit_nl(off);
				if (kc_is_binary((const unsigned char *)kc_scratch,
				                 (int)n)) {
					off = kc_info_emit_str(off, "  -- hex ----\n");
					off = kc_info_emit_hex_dump(off,
					        (const unsigned char *)kc_scratch,
					        (int)n);
				} else {
					off = kc_info_emit_str(off, "  -- content ----\n");
					/* Copy text bytes verbatim;
					 * kc_info_print_line strips
					 * non-printables and splits on \n. */
					int cap = n > 512 ? 512 : (int)n;
					for (int k = 0; k < cap
					        && off < (int)sizeof(kc_info_buf) - 1;
					        k++)
						kc_info_buf[off++] = kc_scratch[k];
				}
			}
		} else {
			off = kc_info_emit_str(off, "  Type: ?\n");
		}
	}

	/* Walk lines.  Output starts at row KC_LIST_TOP and runs for
	 * KC_LIST_ROWS rows. */
	int row = KC_LIST_TOP;
	int i = 0;
	while (row < KC_LIST_TOP + KC_LIST_ROWS && i < off) {
		/* Extract one line into a small temporary buffer. */
		char tmp[KC_RIGHT_W + 1];
		int  tlen = 0;
		while (i < off && kc_info_buf[i] != '\n'
		       && tlen < (int)sizeof(tmp) - 1) {
			tmp[tlen++] = kc_info_buf[i++];
		}
		tmp[tlen] = '\0';
		/* Skip the \n separator if present. */
		if (i < off && kc_info_buf[i] == '\n') i++;
		kc_info_print_line(row++, tmp);
	}
	/* Blank remaining rows. */
	while (row < KC_LIST_TOP + KC_LIST_ROWS) {
		kc_info_blank(row++);
	}
}

/* Paint one solid row of horizontal box character between two columns
 * (inclusive of both ends, but the caller positions the cursor). */
static void kc_render_h_run(int cells)
{
	for (int i = 0; i < cells; i++) cwrite(KC_BOX_H);
}

/* Top border: ╔══<lpath truncated>══╦══ Info ══╗
 *
 * The path slot gets " <path> " with horizontal fills on either side;
 * if the path is too long for the slot, we cut from the LEFT (keeping
 * the tail components visible) with a leading "...". */
static void kc_render_frame_top(void)
{
	kc_attr_normal();
	kc_move(KC_TOP_ROW, KC_BORDER_L_COL);
	cwrite(KC_BOX_TL);

	/* --- left section: KC_LEFT_W cells (= 38) --- */
	const char *lpath = kc_panels[0].path;
	int llen = (int)ustrlen(lpath);
	int lslot = KC_LEFT_W - 2;	/* horizontal pad on each side >=1 */
	int ldisp;
	const char *lshow = lpath;
	int truncated = 0;
	if (llen + 2 > lslot) {
		/* Need to drop the leading components.  Show "..." + tail. */
		lshow = lpath + (llen - (lslot - 5));
		truncated = 1;
		ldisp = lslot - 2;	/* the " <text> " consumes lslot-2 box chars */
	} else {
		ldisp = llen + 2;
	}
	int lfill_left  = (KC_LEFT_W - ldisp) / 2;
	int lfill_right = KC_LEFT_W - ldisp - lfill_left;
	kc_render_h_run(lfill_left);
	cputc(' ');
	if (truncated) cwrite("...");
	cwrite(lshow);
	cputc(' ');
	kc_render_h_run(lfill_right);

	/* --- middle T --- */
	cwrite(KC_BOX_TD);

	/* --- right section: KC_RIGHT_W cells (= 39), title "Info" --- */
	const char *rtitle = " Info ";
	int rlen = 6;
	int rfill_left  = (KC_RIGHT_W - rlen) / 2;
	int rfill_right = KC_RIGHT_W - rlen - rfill_left;
	kc_render_h_run(rfill_left);
	cwrite(rtitle);
	kc_render_h_run(rfill_right);

	cwrite(KC_BOX_TR);
}

static void kc_render_frame_bottom(void)
{
	kc_attr_normal();
	kc_move(KC_BOT_ROW, KC_BORDER_L_COL);
	cwrite(KC_BOX_BL);
	kc_render_h_run(KC_LEFT_W);
	cwrite(KC_BOX_BU);
	kc_render_h_run(KC_RIGHT_W);
	cwrite(KC_BOX_BR);
}

/* Draw the three vertical bars (left, middle, right) on each list row.
 * Called once during a full repaint; the cell renderer writes into
 * the space between them. */
static void kc_render_frame_sides(void)
{
	kc_attr_normal();
	for (int r = KC_LIST_TOP; r <= KC_LIST_BOT; r++) {
		kc_move(r, KC_BORDER_L_COL); cwrite(KC_BOX_V);
		kc_move(r, KC_BORDER_M_COL); cwrite(KC_BOX_V);
		kc_move(r, KC_BORDER_R_COL); cwrite(KC_BOX_V);
	}
}

static void kc_render_status(void)
{
	kc_attr_status();
	kc_move(KC_INFO_ROW, 1);
	for (int i = 0; i < KC_W; i++) cputc(' ');

	struct kc_panel *p = &kc_panels[kc_active];
	if (p->cur >= p->n) return;
	struct kc_ent *e = &p->ents[p->cur];

	kc_move(KC_INFO_ROW, 2);
	cwrite(" ");
	cwrite(p->path);
	if (!(p->path[0] == '/' && p->path[1] == '\0')) cwrite("/");
	cwrite(e->name);
	cwrite("    ");
	if (e->type == 'd') {
		cwrite("(directory)");
	} else if (e->type == 'c') {
		cwrite("(char device)");
	} else if (e->type == 'b') {
		cwrite("(block device)");
	} else {
		kc_print_size(e->size, 0);
		cwrite(" bytes");
	}
}

/*
 * Footer.  Classic NC: each cell shows "N<Label>" in alternating
 * black-on-white (number) and white-on-cyan (label) chunks.  Two
 * chars per number + 5-6 per label, 10 cells, fits ~80 cols.
 */
struct kc_fkey { const char *num; const char *lbl; };
static const struct kc_fkey kc_fkeys[10] = {
	{ "1", "Help"  }, { "2", "Menu"  }, { "3", "View"  }, { "4", "Edit"  },
	{ "5", "Copy"  }, { "6", "RenMv" }, { "7", "Mkdir" }, { "8", "Delet" },
	/*
	 * Last cell advertises `q` rather than `10` because terminal
	 * emulators and QEMU's GTK display routinely steal F10 for
	 * their own menu shortcut.  F10 still works on terminals that
	 * pass it through; q / Esc / Ctrl-X are the always-available
	 * alternatives, and they're all wired up in kc_handle_key.
	 */
	{ "9", "PullDn"}, { "q", "Quit"  },
};

static void kc_render_footer(void)
{
	kc_move(KC_FN_ROW, 1);
	for (int i = 0; i < 10; i++) {
		kc_attr_fkey_num();
		cwrite(kc_fkeys[i].num);
		kc_attr_footer();
		cwrite(kc_fkeys[i].lbl);
		/* one trailing space between cells (and at line end) */
		cputc(' ');
	}
	/* fill to col 80 */
	kc_attr_footer();
	/* we don't know exact width consumed; just pad enough */
	kc_pad_spaces(4);
	kc_attr_reset();
}

static void kc_render_all(void)
{
	kc_hide_cursor();
	/* Paint a full blue background first so any uncovered cells inherit. */
	kc_attr_normal();
	kc_clear_screen();
	for (int r = 1; r <= KC_H; r++) {
		kc_move(r, 1);
		for (int c = 0; c < KC_W; c++) cputc(' ');
	}

	kc_render_frame_top();
	kc_render_frame_sides();
	kc_render_panel(0);
	kc_render_info();
	kc_render_frame_bottom();
	kc_render_status();
	kc_render_footer();

	kc_attr_reset();
	kc_flush();
}

/* Light repaint after arrow-key moves: panels + info + status.  The
 * frame stays put. */
static void kc_repaint_dynamic(void)
{
	kc_render_frame_top();		/* path may have changed */
	kc_render_panel(0);
	kc_render_info();
	kc_render_status();
	kc_attr_reset();
	kc_flush();
}

/* ---- input ------------------------------------------------------------- */

/*
 * Key codes returned by kc_read_key.  Negative space for the special
 * keys so positive returns map directly to ASCII bytes.
 */
#define KC_KEY_NONE     0
#define KC_KEY_UP      -1
#define KC_KEY_DOWN    -2
#define KC_KEY_LEFT    -3
#define KC_KEY_RIGHT   -4
#define KC_KEY_PGUP    -5
#define KC_KEY_PGDN    -6
#define KC_KEY_HOME    -7
#define KC_KEY_END     -8
#define KC_KEY_F1      -11
#define KC_KEY_F2      -12
#define KC_KEY_F3      -13
#define KC_KEY_F4      -14
#define KC_KEY_F5      -15
#define KC_KEY_F6      -16
#define KC_KEY_F7      -17
#define KC_KEY_F8      -18
#define KC_KEY_F9      -19
#define KC_KEY_F10     -20

/*
 * read_one() blocks on a single byte from /dev/console.  We parse:
 *
 *   ESC                              -> Esc
 *   ESC [ A/B/C/D                    -> arrows
 *   ESC [ 5 ~  / 6 ~                 -> PgUp / PgDn
 *   ESC [ H / F   ESC [ 1 ~ / 4 ~    -> Home / End
 *   ESC O P/Q/R/S                    -> F1..F4 (VT100)
 *   ESC [ 1 1 ~ ... 2 1 ~            -> F1..F10 (xterm)
 *
 * Anything unrecognized after ESC returns ESC itself.
 */
static int kc_read_key(void)
{
	int c = read_one();
	if (c < 0) return KC_KEY_NONE;
	if (c != 0x1B) return c;

	c = read_one();
	if (c == '[') {
		int n1 = read_one();
		switch (n1) {
		case 'A': return KC_KEY_UP;
		case 'B': return KC_KEY_DOWN;
		case 'C': return KC_KEY_RIGHT;
		case 'D': return KC_KEY_LEFT;
		case 'H': return KC_KEY_HOME;
		case 'F': return KC_KEY_END;
		}
		/* numeric: collect up to '~' */
		int num = 0;
		while (n1 >= '0' && n1 <= '9') {
			num = num * 10 + (n1 - '0');
			n1 = read_one();
		}
		/* n1 should now be '~' (or ';' for modified -- ignore) */
		if (n1 == ';') {
			/* swallow modifier digits */
			int junk = read_one();
			while (junk >= '0' && junk <= '9') junk = read_one();
			n1 = junk;
		}
		if (n1 == '~') {
			switch (num) {
			case 1:  return KC_KEY_HOME;
			case 4:  return KC_KEY_END;
			case 5:  return KC_KEY_PGUP;
			case 6:  return KC_KEY_PGDN;
			case 11: return KC_KEY_F1;
			case 12: return KC_KEY_F2;
			case 13: return KC_KEY_F3;
			case 14: return KC_KEY_F4;
			case 15: return KC_KEY_F5;
			case 17: return KC_KEY_F6;	/* 16 is skipped on xterm */
			case 18: return KC_KEY_F7;
			case 19: return KC_KEY_F8;
			case 20: return KC_KEY_F9;
			case 21: return KC_KEY_F10;
			}
		}
		return 0x1B;	/* unrecognized CSI -- treat as bare Esc */
	} else if (c == 'O') {
		int n = read_one();
		switch (n) {
		case 'P': return KC_KEY_F1;
		case 'Q': return KC_KEY_F2;
		case 'R': return KC_KEY_F3;
		case 'S': return KC_KEY_F4;
		case 'H': return KC_KEY_HOME;
		case 'F': return KC_KEY_END;
		}
		return 0x1B;
	}
	/* bare Esc */
	return 0x1B;
}

/* ---- dialog ------------------------------------------------------------ */

/*
 * Prompt at the status row for a line of input.  Returns 1 on Enter,
 * 0 on Esc (cancelled).  Echoes characters; backspace edits.
 */
static int kc_prompt(const char *label, char *out, size_t cap)
{
	kc_show_cursor();
	kc_attr_status();
	kc_move(KC_INFO_ROW, 1);
	for (int i = 0; i < KC_W; i++) cputc(' ');
	kc_move(KC_INFO_ROW, 2);
	cwrite(label);
	cwrite(": ");

	size_t i = 0;
	out[0] = '\0';
	for (;;) {
		kc_flush();
		int k = kc_read_key();
		if (k == '\r' || k == '\n') { out[i] = '\0'; kc_hide_cursor(); kc_flush(); return 1; }
		if (k == 0x1B)              { out[0] = '\0'; kc_hide_cursor(); kc_flush(); return 0; }
		if (k == 0x7F || k == 0x08) {
			if (i > 0) { i--; cwrite("\b \b"); }
			continue;
		}
		if (k < 0) continue;	/* ignore special keys */
		if (i + 1 >= cap) continue;
		out[i++] = (char)k;
		cputc((char)k);
	}
}

/*
 * Confirm dialog: shows "<msg>  [Y/n]".  Default Yes on Enter.
 */
static int kc_confirm(const char *msg)
{
	kc_attr_status();
	kc_move(KC_INFO_ROW, 1);
	for (int i = 0; i < KC_W; i++) cputc(' ');
	kc_move(KC_INFO_ROW, 2);
	cwrite(msg);
	cwrite("  [Y/n]: ");
	kc_show_cursor();
	kc_flush();
	int k = kc_read_key();
	kc_hide_cursor();
	kc_flush();
	if (k == 'n' || k == 'N' || k == 0x1B) return 0;
	return 1;
}

static void kc_flash_msg(const char *msg)
{
	kc_attr_status();
	kc_move(KC_INFO_ROW, 1);
	for (int i = 0; i < KC_W; i++) cputc(' ');
	kc_move(KC_INFO_ROW, 2);
	cwrite(msg);
	cwrite("   -- press any key --");
	kc_flush();
	(void)kc_read_key();
}

/* ---- file operations --------------------------------------------------- */

/* Build absolute path "<panel.path>/<entry name>" into out. */
static void kc_make_path(const struct kc_panel *p, const char *name, char *out, size_t cap)
{
	size_t i = 0;
	while (p->path[i] && i + 1 < cap) { out[i] = p->path[i]; i++; }
	if (!(i == 1 && out[0] == '/') && i + 1 < cap) out[i++] = '/';
	size_t j = 0;
	while (name[j] && i + 1 < cap) out[i++] = name[j++];
	out[i] = '\0';
	path_canon(out);
}

static void kc_op_enter(void)
{
	struct kc_panel *p = &kc_panels[kc_active];
	if (p->cur >= p->n) return;
	struct kc_ent *e = &p->ents[p->cur];

	if (e->type == 'd') {
		char np[128];
		kc_make_path(p, e->name, np, sizeof(np));
		kc_copy_str(p->path, np, sizeof(p->path));
		kc_panel_load(p);
	} else if (e->type == 'r') {
		/* Treat Enter on a file as F3 View. */
		kc_op_view();
	}
}

static void kc_op_view(void)
{
	struct kc_panel *p = &kc_panels[kc_active];
	if (p->cur >= p->n) return;
	struct kc_ent *e = &p->ents[p->cur];
	if (e->type != 'r') {
		kc_flash_msg("kc: not a regular file");
		return;
	}

	char path[128];
	kc_make_path(p, e->name, path, sizeof(path));

	long fd = sys_open(path, 0);
	if (fd < 0) { kc_flash_msg("kc: open failed"); return; }

	kc_attr_reset();
	kc_clear_screen();
	kc_show_cursor();

	cwrite("--- "); cwrite(path); cwrite(" ---\r\n");

	char buf[256];
	long n;
	int lines = 0;
	while ((n = sys_read((int)fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < n; i++) {
			char c = buf[i];
			if (c == '\n') { cputc('\r'); cputc('\n'); lines++; }
			else           { cputc(c); }
		}
		if (lines >= 22) {
			cwrite("\r\n--- more --- ");
			kc_flush();
			int k = kc_read_key();
			if (k == 'q' || k == 0x1B) break;
			lines = 0;
		}
	}
	sys_close((int)fd);
	cwrite("\r\n--- end --- press any key");
	kc_flush();
	(void)kc_read_key();
	kc_render_all();
}

static void kc_op_mkdir(void)
{
	char name[KC_NAME_MAX];
	if (!kc_prompt("Create directory", name, sizeof(name))) {
		kc_render_status();
		return;
	}
	if (name[0] == '\0') { kc_render_status(); return; }

	struct kc_panel *p = &kc_panels[kc_active];
	char path[128];
	kc_make_path(p, name, path, sizeof(path));
	if (sys_mkdir(path) < 0) {
		kc_flash_msg("kc: mkdir failed");
	} else {
		kc_panel_load(p);
		kc_panel_load(&kc_panels[1 - kc_active]);	/* refresh both, in case same dir */
	}
	kc_render_all();
}

static void kc_op_delete(void)
{
	struct kc_panel *p = &kc_panels[kc_active];
	if (p->cur >= p->n) return;
	struct kc_ent *e = &p->ents[p->cur];
	if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0') {
		kc_flash_msg("kc: refusing to delete ..");
		return;
	}

	char buf[160];
	const char *p0 = "Delete '";
	int k = 0;
	while (*p0) buf[k++] = *p0++;
	int j = 0;
	while (e->name[j] && k + 4 < (int)sizeof(buf)) buf[k++] = e->name[j++];
	const char *p1 = "'?";
	while (*p1) buf[k++] = *p1++;
	buf[k] = '\0';

	if (!kc_confirm(buf)) { kc_render_status(); return; }

	char path[128];
	kc_make_path(p, e->name, path, sizeof(path));

	long r;
	if (e->type == 'd') r = sys_rmdir(path);
	else                r = sys_unlink(path);

	if (r < 0) {
		kc_flash_msg("kc: delete failed");
	} else {
		kc_panel_load(p);
		kc_panel_load(&kc_panels[1 - kc_active]);
	}
	kc_render_all();
}

/* Copy file at src to file at dst.  Returns 0 on success, -1 on error. */
static int kc_copy_file(const char *src, const char *dst)
{
	long sfd = sys_open(src, 0);
	if (sfd < 0) return -1;

	if (sys_creat(dst) < 0) {
		/* maybe already exists -- O_TRUNC will overwrite */
	}
	long dfd = sys_open(dst, O_TRUNC);
	if (dfd < 0) { sys_close((int)sfd); return -1; }

	char buf[512];
	long n;
	while ((n = sys_read((int)sfd, buf, sizeof(buf))) > 0) {
		long w = sys_write((int)dfd, buf, (size_t)n);
		if (w != n) { sys_close((int)sfd); sys_close((int)dfd); return -1; }
	}
	sys_close((int)sfd);
	sys_close((int)dfd);
	return 0;
}

static void kc_op_copy(void)
{
	struct kc_panel *src = &kc_panels[kc_active];
	struct kc_panel *dst = &kc_panels[1 - kc_active];
	if (src->cur >= src->n) return;
	struct kc_ent *e = &src->ents[src->cur];
	if (e->type != 'r') {
		kc_flash_msg("kc: only regular files can be copied");
		return;
	}

	char proposed[128];
	kc_make_path(dst, e->name, proposed, sizeof(proposed));

	char prompt_msg[160];
	int k = 0;
	const char *p0 = "Copy to (default ";
	while (*p0) prompt_msg[k++] = *p0++;
	int j = 0;
	while (proposed[j] && k + 2 < (int)sizeof(prompt_msg)) prompt_msg[k++] = proposed[j++];
	prompt_msg[k++] = ')';
	prompt_msg[k]   = '\0';

	char user_in[128];
	if (!kc_prompt(prompt_msg, user_in, sizeof(user_in))) {
		kc_render_status();
		return;
	}

	char dst_path[128];
	if (user_in[0] == '\0') {
		kc_copy_str(dst_path, proposed, sizeof(dst_path));
	} else if (user_in[0] == '/') {
		kc_copy_str(dst_path, user_in, sizeof(dst_path));
		path_canon(dst_path);
	} else {
		kc_make_path(dst, user_in, dst_path, sizeof(dst_path));
	}

	char src_path[128];
	kc_make_path(src, e->name, src_path, sizeof(src_path));

	if (kc_copy_file(src_path, dst_path) < 0) {
		kc_flash_msg("kc: copy failed");
	} else {
		kc_panel_load(dst);
		kc_panel_load(src);
	}
	kc_render_all();
}

static void kc_op_move(void)
{
	/*
	 * Real Unix rename(2) doesn't exist in our syscall table yet,
	 * so move = copy + unlink.  Works within and across "directories"
	 * (we only have one filesystem -- kfs -- so it's all the same
	 * inode space anyway).
	 */
	struct kc_panel *src = &kc_panels[kc_active];
	struct kc_panel *dst = &kc_panels[1 - kc_active];
	if (src->cur >= src->n) return;
	struct kc_ent *e = &src->ents[src->cur];
	if (e->type != 'r') {
		kc_flash_msg("kc: only regular files can be moved");
		return;
	}

	char proposed[128];
	kc_make_path(dst, e->name, proposed, sizeof(proposed));

	char prompt_msg[160];
	int k = 0;
	const char *p0 = "Move/rename to (default ";
	while (*p0) prompt_msg[k++] = *p0++;
	int j = 0;
	while (proposed[j] && k + 2 < (int)sizeof(prompt_msg)) prompt_msg[k++] = proposed[j++];
	prompt_msg[k++] = ')';
	prompt_msg[k]   = '\0';

	char user_in[128];
	if (!kc_prompt(prompt_msg, user_in, sizeof(user_in))) {
		kc_render_status();
		return;
	}

	char dst_path[128];
	if (user_in[0] == '\0') {
		kc_copy_str(dst_path, proposed, sizeof(dst_path));
	} else if (user_in[0] == '/') {
		kc_copy_str(dst_path, user_in, sizeof(dst_path));
		path_canon(dst_path);
	} else {
		kc_make_path(src, user_in, dst_path, sizeof(dst_path));
	}

	char src_path[128];
	kc_make_path(src, e->name, src_path, sizeof(src_path));

	if (kc_copy_file(src_path, dst_path) < 0) {
		kc_flash_msg("kc: move failed (copy step)");
		kc_render_all();
		return;
	}
	if (sys_unlink(src_path) < 0) {
		kc_flash_msg("kc: move failed (unlink step)");
	} else {
		kc_panel_load(src);
		kc_panel_load(dst);
	}
	kc_render_all();
}

static void kc_op_edit(void)
{
	/*
	 * Hand the screen to vi for an interactive edit.  vi is a
	 * synchronous function call in the same address space; when it
	 * returns (`:q` / `:wq`) we re-render kc from scratch.  Its
	 * globals (`struct vi vi`) are disjoint from kc's, so nothing
	 * gets clobbered on the way in or out.
	 */
	struct kc_panel *p = &kc_panels[kc_active];
	if (p->cur >= p->n) return;
	struct kc_ent *e = &p->ents[p->cur];
	if (e->type != 'r') {
		kc_flash_msg("kc: only regular files can be edited");
		return;
	}

	char path[128];
	kc_make_path(p, e->name, path, sizeof(path));

	/* Drain our buffer before vi starts writing -- otherwise kc's
	 * pending bytes would appear on top of vi's first frame. */
	kc_flush();

	char *argv_vi[2] = { (char *)"vi", path };
	cmd_vi(2, argv_vi);

	/* The file's size (and whether it still exists) may have
	 * changed; refresh both panels so the listing reflects it. */
	kc_panel_load(&kc_panels[0]);
	kc_panel_load(&kc_panels[1]);
	kc_render_all();
}

static void kc_op_help(void)
{
	kc_attr_reset();
	kc_clear_screen();
	kc_show_cursor();
	cwrite("\r\nkc -- kappara commander\r\n");
	cwrite("------------------------\r\n\r\n");
	cwrite("  Arrows         move cursor\r\n");
	cwrite("  Tab            switch active panel\r\n");
	cwrite("  Enter          enter directory / view file\r\n");
	cwrite("  PgUp/PgDn      scroll a page\r\n");
	cwrite("  Home/End       first / last entry\r\n");
	cwrite("\r\n");
	cwrite("  F1     this help                F6   move / rename\r\n");
	cwrite("  F3     view file                F7   make directory\r\n");
	cwrite("  F5     copy file to other panel F8   delete\r\n");
	cwrite("  q / Q / Esc / Ctrl-X / F10      quit\r\n");
	cwrite("\r\n");
	cwrite("Note: F10 is often eaten by the terminal or QEMU GTK display;\r\n");
	cwrite("`q` and `Ctrl-X` are reliable everywhere.\r\n");
	cwrite("\r\n");
	cwrite("Number keys 1..0 also fire F1..F10 if your terminal doesn't\r\n");
	cwrite("send the function-key sequences.\r\n");
	cwrite("\r\n-- press any key --");
	kc_flush();
	(void)kc_read_key();
	kc_render_all();
}

/* ---- main loop --------------------------------------------------------- */

static void kc_init_panels(void)
{
	kc_copy_str(kc_panels[0].path, cwd, sizeof(kc_panels[0].path));
	kc_copy_str(kc_panels[1].path, "/", sizeof(kc_panels[1].path));
	kc_active = 0;
	kc_quit   = 0;
	kc_panel_load(&kc_panels[0]);
	kc_panel_load(&kc_panels[1]);
}

static void kc_clamp_cursor(struct kc_panel *p)
{
	if (p->cur < 0)      p->cur = 0;
	if (p->cur >= p->n)  p->cur = (p->n > 0) ? p->n - 1 : 0;
	if (p->cur < p->top) p->top = p->cur;
	if (p->cur >= p->top + KC_LIST_ROWS)
		p->top = p->cur - KC_LIST_ROWS + 1;
	if (p->top < 0) p->top = 0;
}

static void kc_handle_key(int k)
{
	struct kc_panel *p = &kc_panels[kc_active];

	switch (k) {
	case KC_KEY_UP:    p->cur--;                       break;
	case KC_KEY_DOWN:  p->cur++;                       break;
	case KC_KEY_PGUP:  p->cur -= KC_LIST_ROWS - 1;     break;
	case KC_KEY_PGDN:  p->cur += KC_LIST_ROWS - 1;     break;
	case KC_KEY_HOME:  p->cur = 0;                     break;
	case KC_KEY_END:   p->cur = p->n - 1;              break;
	case '\t':         kc_active = 1 - kc_active;      break;
	case '\r':
	case '\n':         kc_op_enter();                  break;

	case KC_KEY_F1: case '1': kc_op_help();   break;
	case KC_KEY_F2: case '2':
		kc_flash_msg("kc: F2 menu not yet implemented");
		break;
	case KC_KEY_F3: case '3': kc_op_view();   break;
	case KC_KEY_F4: case '4': kc_op_edit();   break;
	case KC_KEY_F5: case '5': kc_op_copy();   break;
	case KC_KEY_F6: case '6': kc_op_move();   break;
	case KC_KEY_F7: case '7': kc_op_mkdir();  break;
	case KC_KEY_F8: case '8': kc_op_delete(); break;
	case KC_KEY_F9: case '9':
		kc_flash_msg("kc: F9 pull-down menu not yet implemented");
		break;
	case KC_KEY_F10: case '0':
	case 'q': case 'Q':
	case 0x18:	/* Ctrl-X -- belt-and-braces exit shortcut for
			 * environments where F10 is eaten by the
			 * terminal emulator / QEMU GTK display. */
	case 0x1B:	/* Esc */
		kc_quit = 1;
		break;
	default:
		break;
	}

	kc_clamp_cursor(p);
	kc_clamp_cursor(&kc_panels[1 - kc_active]);
}

static void cmd_kc(int argc, char *argv[])
{
	(void)argc; (void)argv;

	/* Inhibit SIGINT while kc owns the screen -- the shell's handler
	 * would print "^C\r\n" over our panels.  F4 (Edit) drops into vi,
	 * which installs its own SIG_IGN on top of ours; both restore on
	 * the way out and we end up back at the shell's original
	 * disposition. */
	struct sigaction saved_sigint;
	editor_suspend_sigint(&saved_sigint);

	kc_init_panels();
	kc_render_all();
	while (!kc_quit) {
		int k = kc_read_key();
		if (k == KC_KEY_NONE) continue;
		kc_handle_key(k);
		kc_repaint_dynamic();
	}
	kc_attr_reset();
	kc_clear_screen();
	kc_show_cursor();
	kc_flush();

	editor_restore_sigint(&saved_sigint);
}

/*
 * Drop the cwrite/cputc macros so init.c's code below this include
 * (dispatch, _start) gets the unbuffered static-function versions
 * again.  Forgetting this would silently buffer the shell prompt
 * and command output -- visible only after the next kc_flush.
 */
#undef cwrite
#undef cputc

/* ===========================================================================
 * UnoDOS/pc64 - unoterm: the VT100/xterm screen emulator.  Contract: unoterm.h.
 *
 * Ported from Portage's TerminalScreen.cs.  Two structural differences, both
 * forced by the target and both improvements:
 *
 *   - NO ALLOCATION.  The caller hands over one block; init() carves it.
 *   - ROWS ARE AN INDIRECTION TABLE.  C# swapped row REFERENCES to scroll;
 *     a C port that memmoved the cells would make every line of shell output
 *     cost a full-screen copy.  row[y] says which physical line is display
 *     line y, so a scroll rotates shorts.
 *
 * This file includes NOTHING.  It is deliberately dependency-free so that
 * tools/unoterm_test.c can build the identical bytes on the dev PC - a
 * terminal emulator whose test builds a DIFFERENT emulator tests nothing,
 * which is the trap the audit note about host harnesses records.
 * ======================================================================== */
#include "unoterm.h"

/* ---- freestanding minimum ------------------------------------------------ */
static void t_memset(void *d, int c, unsigned long n)
{ unsigned char *p = (unsigned char *)d; while (n--) *p++ = (unsigned char)c; }

static int t_min(int a, int b) { return a < b ? a : b; }
static int t_max(int a, int b) { return a > b ? a : b; }
static int t_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

enum { ST_GROUND = 0, ST_ESC, ST_CSI, ST_OSC, ST_CHARSET };

/* ---- geometry ------------------------------------------------------------ */
static unoterm_cell *line_of(const unoterm *t, int y)
{
    unoterm_cell *base = t->alt_screen ? t->alt : t->store;
    const short *map = t->alt_screen ? t->arow : t->row;
    return base + (long)map[y] * t->cols;
}

static void blank_cell(const unoterm *t, unoterm_cell *c)
{
    (void)t;
    c->ch = ' '; c->pad = 0; c->fg = UNOTERM_DEFAULT_COLOR;
    c->bg = UNOTERM_DEFAULT_COLOR; c->attr = 0;
    c->pad2[0] = c->pad2[1] = c->pad2[2] = 0;
}

static void blank_line(const unoterm *t, int y)
{
    unoterm_cell *r = line_of(t, y);
    int x;
    for (x = 0; x < t->cols; x++) blank_cell(t, &r[x]);
}

unsigned long unoterm_memneed(int cols, int rows, int sb)
{
    unsigned long cell = sizeof(unoterm_cell);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (sb < 0) sb = 0;
    return (unsigned long)cols * (unsigned long)rows * cell * 2ul
         + (unsigned long)cols * (unsigned long)sb * cell
         + (unsigned long)rows * sizeof(short) * 2ul
         + 64ul;                       /* alignment slack */
}

static void reset_state(unoterm *t)
{
    int y;
    t->cx = t->cy = t->saved_cx = t->saved_cy = 0;
    t->scroll_top = 0;
    t->scroll_bottom = t->rows - 1;
    t->wrap_next = 0;
    t->fg = t->bg = UNOTERM_DEFAULT_COLOR;
    t->attr = 0;
    t->state = ST_GROUND;
    t->nparams = t->param_seen = t->csi_private = 0;
    t->osclen = 0;
    t->utf_acc = 0; t->utf_left = 0;
    t->alt_screen = 0;
    t->cursor_visible = 1;
    t->app_cursor_keys = 0;
    t->bracketed_paste = 0;
    t->dirty = 1;
    for (y = 0; y < t->rows; y++) { t->row[y] = (short)y; t->arow[y] = (short)y; }
    for (y = 0; y < t->rows; y++) blank_line(t, y);
    t->alt_screen = 1;
    for (y = 0; y < t->rows; y++) blank_line(t, y);
    t->alt_screen = 0;
}

int unoterm_init(unoterm *t, void *mem, unsigned long memlen,
                 int cols, int rows, int sb)
{
    unsigned char *p = (unsigned char *)mem;
    unsigned long need;

    if (!t || !mem) return 0;
    cols = t_max(1, cols); rows = t_max(1, rows);
    if (sb < 0) sb = 0;
    need = unoterm_memneed(cols, rows, sb);
    if (memlen < need) return 0;

    t_memset(t, 0, sizeof *t);
    t->cols = cols; t->rows = rows; t->sb_rows = sb;
    t->cap_cells = cols * rows;             /* the reservation, fixed for life */

    /* Carve in descending alignment order so no member needs padding between
     * it and the next: cells (8-aligned) first, the two short tables last. */
    t->store = (unoterm_cell *)p; p += (unsigned long)cols * rows * sizeof(unoterm_cell);
    t->alt   = (unoterm_cell *)p; p += (unsigned long)cols * rows * sizeof(unoterm_cell);
    t->scroll = sb ? (unoterm_cell *)p : 0;
    p += (unsigned long)cols * sb * sizeof(unoterm_cell);
    t->row  = (short *)p; p += (unsigned long)rows * sizeof(short);
    t->arow = (short *)p;

    reset_state(t);
    return 1;
}

void unoterm_resize(unoterm *t, int cols, int rows)
{
    /* The block was carved for the ORIGINAL geometry, so a resize can shrink
     * freely and can only grow back to what was reserved.  Refusing to grow
     * past the reservation is the honest behaviour: the alternative is writing
     * past the caller's buffer, which is a memory-safety bug wearing a
     * convenience feature's clothes. */
    int maxcells = t->cap_cells, y;
    cols = t_max(1, cols); rows = t_max(1, rows);
    if (cols * rows > maxcells) {
        rows = maxcells / cols;
        if (rows < 1) { rows = 1; cols = maxcells; }
    }
    if (cols == t->cols && rows == t->rows) return;
    t->cols = cols; t->rows = rows;
    for (y = 0; y < rows; y++) { t->row[y] = (short)y; t->arow[y] = (short)y; }
    t->scroll_top = 0;
    t->scroll_bottom = rows - 1;
    t->cx = t_min(t->cx, cols - 1);
    t->cy = t_min(t->cy, rows - 1);
    t->wrap_next = 0;
    t->dirty = 1;
}

/* ---- scrolling ----------------------------------------------------------- */
static void push_scrollback(unoterm *t, const unoterm_cell *src)
{
    unoterm_cell *dst;
    int x;
    if (!t->scroll || t->sb_rows <= 0) return;
    dst = t->scroll + (long)t->sb_head * t->cols;
    for (x = 0; x < t->cols; x++) dst[x] = src[x];
    t->sb_head = (t->sb_head + 1) % t->sb_rows;
    if (t->sb_count < t->sb_rows) t->sb_count++;
}

static void scroll_up(unoterm *t)
{
    short *map = t->alt_screen ? t->arow : t->row;
    short off = map[t->scroll_top];
    int y;
    /* Only the PRIMARY screen scrolling from the true top feeds scrollback:
     * a scroll region inside vim is not history, and treating it as history
     * fills the buffer with fragments of a full-screen app. */
    if (!t->alt_screen && t->scroll_top == 0)
        push_scrollback(t, t->store + (long)off * t->cols);
    for (y = t->scroll_top; y < t->scroll_bottom; y++) map[y] = map[y + 1];
    map[t->scroll_bottom] = off;
    blank_line(t, t->scroll_bottom);
}

static void scroll_down(unoterm *t)
{
    short *map = t->alt_screen ? t->arow : t->row;
    short off = map[t->scroll_bottom];
    int y;
    for (y = t->scroll_bottom; y > t->scroll_top; y--) map[y] = map[y - 1];
    map[t->scroll_top] = off;
    blank_line(t, t->scroll_top);
}

static void line_feed(unoterm *t)
{
    if (t->cy == t->scroll_bottom) scroll_up(t);
    else if (t->cy < t->rows - 1) t->cy++;
}

static void reverse_line_feed(unoterm *t)
{
    if (t->cy == t->scroll_top) scroll_down(t);
    else if (t->cy > 0) t->cy--;
}

/* ---- printing ------------------------------------------------------------ */
static void put_char(unoterm *t, unsigned cp)
{
    unoterm_cell *r;
    if (t->wrap_next) { t->cx = 0; line_feed(t); t->wrap_next = 0; }
    r = line_of(t, t->cy);
    r[t->cx].ch = (unsigned short)(cp > 0xFFFFu ? '?' : cp);
    r[t->cx].fg = t->fg;
    r[t->cx].bg = t->bg;
    r[t->cx].attr = t->attr;
    if (t->cx >= t->cols - 1) t->wrap_next = 1;
    else t->cx++;
}

/* ---- erase / insert / delete --------------------------------------------- */
static void erase_line(unoterm *t, int mode)
{
    unoterm_cell *r = line_of(t, t->cy);
    int from = mode == 0 ? t->cx : 0;
    int to   = mode == 1 ? t->cx : t->cols - 1;
    int x;
    for (x = from; x <= to && x < t->cols; x++) blank_cell(t, &r[x]);
}

static void erase_display(unoterm *t, int mode)
{
    int y;
    if (mode == 0) {
        erase_line(t, 0);
        for (y = t->cy + 1; y < t->rows; y++) blank_line(t, y);
    } else if (mode == 1) {
        erase_line(t, 1);
        for (y = 0; y < t->cy; y++) blank_line(t, y);
    } else {
        for (y = 0; y < t->rows; y++) blank_line(t, y);
    }
}

static void erase_chars(unoterm *t, int n)
{
    unoterm_cell *r = line_of(t, t->cy);
    int i;
    for (i = 0; i < n && t->cx + i < t->cols; i++) blank_cell(t, &r[t->cx + i]);
}

static void insert_chars(unoterm *t, int n)
{
    unoterm_cell *r = line_of(t, t->cy);
    int x;
    for (x = t->cols - 1; x >= t->cx; x--) {
        if (x - n >= t->cx) r[x] = r[x - n];
        else blank_cell(t, &r[x]);
    }
}

static void delete_chars(unoterm *t, int n)
{
    unoterm_cell *r = line_of(t, t->cy);
    int x;
    for (x = t->cx; x < t->cols; x++) {
        if (x + n < t->cols) r[x] = r[x + n];
        else blank_cell(t, &r[x]);
    }
}

static void insert_lines(unoterm *t, int n)
{
    short *map = t->alt_screen ? t->arow : t->row;
    int i, y;
    if (t->cy < t->scroll_top || t->cy > t->scroll_bottom) return;
    for (i = 0; i < n; i++) {
        short off = map[t->scroll_bottom];
        for (y = t->scroll_bottom; y > t->cy; y--) map[y] = map[y - 1];
        map[t->cy] = off;
        blank_line(t, t->cy);
    }
}

static void delete_lines(unoterm *t, int n)
{
    short *map = t->alt_screen ? t->arow : t->row;
    int i, y;
    if (t->cy < t->scroll_top || t->cy > t->scroll_bottom) return;
    for (i = 0; i < n; i++) {
        short off = map[t->cy];
        for (y = t->cy; y < t->scroll_bottom; y++) map[y] = map[y + 1];
        map[t->scroll_bottom] = off;
        blank_line(t, t->scroll_bottom);
    }
}

/* ---- SGR ----------------------------------------------------------------- */
static int extended_color(const int *p, int n, int i, int *slot)
{
    if (i + 1 >= n) return i;
    if (p[i + 1] == 5 && i + 2 < n) { *slot = p[i + 2] & 0xff; return i + 2; }
    if (p[i + 1] == 2 && i + 4 < n) {
        *slot = UNOTERM_RGB_FLAG | ((p[i + 2] & 0xff) << 16) |
                ((p[i + 3] & 0xff) << 8) | (p[i + 4] & 0xff);
        return i + 4;
    }
    return i + 1;
}

static void apply_sgr(unoterm *t)
{
    int i, n = t->nparams;
    if (n == 0) { t->fg = t->bg = UNOTERM_DEFAULT_COLOR; t->attr = 0; return; }
    for (i = 0; i < n; i++) {
        int c = t->params[i];
        if (c == 0)            { t->fg = t->bg = UNOTERM_DEFAULT_COLOR; t->attr = 0; }
        else if (c == 1)       t->attr |= UNOTERM_BOLD;
        else if (c == 4)       t->attr |= UNOTERM_UNDERLINE;
        else if (c == 7)       t->attr |= UNOTERM_INVERSE;
        else if (c == 22)      t->attr &= (unsigned char)~UNOTERM_BOLD;
        else if (c == 24)      t->attr &= (unsigned char)~UNOTERM_UNDERLINE;
        else if (c == 27)      t->attr &= (unsigned char)~UNOTERM_INVERSE;
        else if (c == 39)      t->fg = UNOTERM_DEFAULT_COLOR;
        else if (c == 49)      t->bg = UNOTERM_DEFAULT_COLOR;
        else if (c >= 30 && c <= 37)   t->fg = c - 30;
        else if (c >= 40 && c <= 47)   t->bg = c - 40;
        else if (c >= 90 && c <= 97)   t->fg = c - 90 + 8;
        else if (c >= 100 && c <= 107) t->bg = c - 100 + 8;
        else if (c == 38)      i = extended_color(t->params, n, i, &t->fg);
        else if (c == 48)      i = extended_color(t->params, n, i, &t->bg);
    }
}

/* ---- modes --------------------------------------------------------------- */
static void switch_alt(unoterm *t, int to_alt, int save_cursor)
{
    int y;
    if (to_alt == t->alt_screen) return;
    if (to_alt) {
        if (save_cursor) { t->saved_cx = t->cx; t->saved_cy = t->cy; }
        t->alt_screen = 1;
        for (y = 0; y < t->rows; y++) t->arow[y] = (short)y;
        for (y = 0; y < t->rows; y++) blank_line(t, y);
        t->cx = t->cy = 0;
    } else {
        t->alt_screen = 0;
        if (save_cursor) { t->cx = t->saved_cx; t->cy = t->saved_cy; }
    }
    t->scroll_top = 0;
    t->scroll_bottom = t->rows - 1;
    t->wrap_next = 0;
}

static void set_mode(unoterm *t, int on)
{
    int i;
    if (!t->csi_private) return;
    for (i = 0; i < t->nparams; i++) {
        switch (t->params[i]) {
        case 1:    t->app_cursor_keys = on; break;
        case 25:   t->cursor_visible = on;  break;
        case 2004: t->bracketed_paste = on; break;
        case 47: case 1047: case 1049:
            switch_alt(t, on, t->params[i] == 1049);
            break;
        default: break;
        }
    }
}

/* ---- OSC ----------------------------------------------------------------- */
static void copy_bounded(char *dst, int cap, const char *src, int n)
{
    int i = 0;
    for (; i < n && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* OSC 0 / 2 = the window title.  OSC 7 = "file://host/path", the shell's
 * working directory, which is what makes "open a file browser here" land in
 * the right place.  Everything else is ignored rather than guessed at. */
static void parse_osc(unoterm *t)
{
    int sep = -1, i;
    for (i = 0; i < t->osclen; i++) if (t->osc[i] == ';') { sep = i; break; }
    if (sep <= 0) return;
    if (sep == 1 && (t->osc[0] == '0' || t->osc[0] == '2')) {
        copy_bounded(t->title, UNOTERM_TITLEMAX, t->osc + 2, t->osclen - 2);
        return;
    }
    if (sep == 1 && t->osc[0] == '7') {
        const char *b = t->osc + 2;
        int n = t->osclen - 2, k;
        /* "file://host/path" - skip the scheme and the authority; the path is
         * from the third slash on.  A payload that is not a file URL is
         * dropped, not half-parsed. */
        if (n > 7 && b[0] == 'f' && b[1] == 'i' && b[2] == 'l' && b[3] == 'e' &&
            b[4] == ':' && b[5] == '/' && b[6] == '/') {
            for (k = 7; k < n && b[k] != '/'; k++) { }
            if (k < n) copy_bounded(t->cwd, UNOTERM_CWDMAX, b + k, n - k);
        }
    }
}

/* ---- the parser ---------------------------------------------------------- */
static void csi_dispatch(unoterm *t, int f)
{
    int n = (t->nparams > 0 && t->params[0] > 0) ? t->params[0] : 1;
    int p0 = t->nparams > 0 ? t->params[0] : 0;
    int p1 = t->nparams > 1 ? t->params[1] : 0;
    int i;

    switch (f) {
    case 'A': t->cy = t_max(t->scroll_top, t->cy - n);    t->wrap_next = 0; break;
    case 'B': t->cy = t_min(t->scroll_bottom, t->cy + n); t->wrap_next = 0; break;
    case 'C': t->cx = t_min(t->cols - 1, t->cx + n);      t->wrap_next = 0; break;
    case 'D': t->cx = t_max(0, t->cx - n);                t->wrap_next = 0; break;
    case 'E': t->cx = 0; t->cy = t_min(t->scroll_bottom, t->cy + n); break;
    case 'F': t->cx = 0; t->cy = t_max(t->scroll_top, t->cy - n);    break;
    case 'G': t->cx = t_clamp(n - 1, 0, t->cols - 1); t->wrap_next = 0; break;
    case 'd': t->cy = t_clamp(n - 1, 0, t->rows - 1); t->wrap_next = 0; break;
    case 'H': case 'f':
        t->cy = t_clamp((p0 > 0 ? p0 : 1) - 1, 0, t->rows - 1);
        t->cx = t_clamp((p1 > 0 ? p1 : 1) - 1, 0, t->cols - 1);
        t->wrap_next = 0;
        break;
    case 'J': erase_display(t, p0); break;
    case 'K': erase_line(t, p0);    break;
    case 'm': apply_sgr(t);         break;
    /* Repeat counts are CLAMPED to the screen height.  A garbled or binary
     * stream can send ESC[99999999S, and past a full screen every extra
     * iteration only re-blanks an already-blank grid - unclamped it spins and
     * freezes the shell's frame loop.  Portage hit this and the note is worth
     * carrying across. */
    case 'L': insert_lines(t, t_min(n, t->rows)); break;
    case 'M': delete_lines(t, t_min(n, t->rows)); break;
    case '@': insert_chars(t, t_min(n, t->cols)); break;
    case 'P': delete_chars(t, t_min(n, t->cols)); break;
    case 'X': erase_chars(t,  t_min(n, t->cols)); break;
    case 'S': for (i = 0; i < t_min(n, t->rows); i++) scroll_up(t);   break;
    case 'T': for (i = 0; i < t_min(n, t->rows); i++) scroll_down(t); break;
    case 'r':
        t->scroll_top    = t_clamp((p0 > 0 ? p0 : 1) - 1, 0, t->rows - 1);
        t->scroll_bottom = t_clamp((p1 > 0 ? p1 : t->rows) - 1, 0, t->rows - 1);
        if (t->scroll_bottom <= t->scroll_top) {
            t->scroll_top = 0; t->scroll_bottom = t->rows - 1;
        }
        t->cx = 0; t->cy = t->scroll_top;
        break;
    case 's': t->saved_cx = t->cx; t->saved_cy = t->cy; break;
    case 'u': t->cx = t->saved_cx; t->cy = t->saved_cy; break;
    case 'h': set_mode(t, 1); break;
    case 'l': set_mode(t, 0); break;
    default: break;
    }
}

static void ground(unoterm *t, unsigned char b)
{
    switch (b) {
    case 0x1b: t->state = ST_ESC; return;
    case 0x07: return;                                          /* BEL       */
    case 0x08: if (t->cx > 0) t->cx--; t->wrap_next = 0; return;/* BS        */
    case 0x09: t->cx = t_min(t->cols - 1, (t->cx / 8 + 1) * 8);
               t->wrap_next = 0; return;                        /* TAB       */
    case 0x0a: case 0x0b: case 0x0c: line_feed(t); return;
    case 0x0d: t->cx = 0; t->wrap_next = 0; return;             /* CR        */
    default: break;
    }
    if (b < 0x20) return;                                       /* other C0  */

    /* UTF-8, decoded incrementally.  A continuation byte that arrives without
     * a lead byte, or a lead whose continuations never come, resets the
     * decoder rather than poisoning every glyph after it - which is what a
     * "resync on the next lead byte" decoder buys you on a live stream. */
    if (b < 0x80) { put_char(t, b); t->utf_left = 0; return; }
    if ((b & 0xC0) == 0x80) {
        if (t->utf_left <= 0) return;                 /* stray continuation  */
        t->utf_acc = (t->utf_acc << 6) | (unsigned)(b & 0x3F);
        if (--t->utf_left == 0) put_char(t, t->utf_acc);
        return;
    }
    if ((b & 0xE0) == 0xC0) { t->utf_acc = b & 0x1Fu; t->utf_left = 1; return; }
    if ((b & 0xF0) == 0xE0) { t->utf_acc = b & 0x0Fu; t->utf_left = 2; return; }
    if ((b & 0xF8) == 0xF0) { t->utf_acc = b & 0x07u; t->utf_left = 3; return; }
    t->utf_left = 0;                                  /* 0xFE/0xFF: invalid  */
}

static void escape(unoterm *t, unsigned char b)
{
    switch (b) {
    case '[': t->nparams = 0; t->param_seen = 0; t->csi_private = 0;
              t->params[0] = 0; t->state = ST_CSI; return;
    case ']': t->osclen = 0; t->state = ST_OSC; return;
    case '7': t->saved_cx = t->cx; t->saved_cy = t->cy; t->state = ST_GROUND; return;
    case '8': t->cx = t->saved_cx; t->cy = t->saved_cy; t->state = ST_GROUND; return;
    case 'M': reverse_line_feed(t); t->state = ST_GROUND; return;
    case 'D': line_feed(t); t->state = ST_GROUND; return;
    case 'E': t->cx = 0; line_feed(t); t->state = ST_GROUND; return;
    case 'c': reset_state(t); return;
    case '(': case ')': case '*': case '+': t->state = ST_CHARSET; return;
    default:  t->state = ST_GROUND; return;
    }
}

static void csi(unoterm *t, unsigned char b)
{
    if (b == '?' || b == '<' || b == '=' || b == '>') { t->csi_private = 1; return; }
    if (b >= '0' && b <= '9') {
        if (t->nparams == 0) t->nparams = 1;
        /* Saturate rather than overflow: a digit flood must not wrap into a
         * negative parameter that then indexes backwards off a row. */
        if (t->params[t->nparams - 1] < 100000)
            t->params[t->nparams - 1] = t->params[t->nparams - 1] * 10 + (b - '0');
        t->param_seen = 1;
        return;
    }
    if (b == ';') {
        if (t->nparams == 0) t->nparams = 1;
        if (t->nparams < UNOTERM_PARAMS) t->params[t->nparams++] = 0;
        return;
    }
    if (b >= 0x20 && b <= 0x2f) return;               /* intermediate bytes  */
    csi_dispatch(t, b);
    t->state = ST_GROUND;
}

static void osc(unoterm *t, unsigned char b)
{
    /* Terminated by BEL, or by ST (ESC \).  Handing the ESC back to the escape
     * state is what makes "ESC \" close the string AND consume the backslash
     * instead of leaving a stray '\' printed on the screen. */
    if (b == 0x07) { parse_osc(t); t->osclen = 0; t->state = ST_GROUND; return; }
    if (b == 0x1b) { parse_osc(t); t->osclen = 0; t->state = ST_ESC;    return; }
    if (t->osclen < (int)sizeof t->osc - 1) t->osc[t->osclen++] = (char)b;
}

void unoterm_feed(unoterm *t, const unsigned char *data, int n)
{
    int i;
    if (!t || !data) return;
    for (i = 0; i < n; i++) {
        unsigned char b = data[i];
        switch (t->state) {
        case ST_GROUND:  ground(t, b);  break;
        case ST_ESC:     escape(t, b);  break;
        case ST_CSI:     csi(t, b);     break;
        case ST_OSC:     osc(t, b);     break;
        default:         t->state = ST_GROUND; break;   /* charset id: eaten */
        }
    }
    t->dirty = 1;
}

/* ---- readers ------------------------------------------------------------- */
static const unoterm_cell kBlank = { ' ', 0, UNOTERM_DEFAULT_COLOR,
                                     UNOTERM_DEFAULT_COLOR, 0, { 0, 0, 0 } };

const unoterm_cell *unoterm_cell_at(const unoterm *t, int x, int y)
{
    if (!t || x < 0 || y < 0 || x >= t->cols || y >= t->rows) return &kBlank;
    return line_of(t, y) + x;
}

const unoterm_cell *unoterm_scrollback(const unoterm *t, int back)
{
    int idx;
    if (!t || !t->scroll || back < 1 || back > t->sb_count) return 0;
    idx = t->sb_head - back;
    while (idx < 0) idx += t->sb_rows;
    return t->scroll + (long)idx * t->cols;
}

int unoterm_scrollback_count(const unoterm *t) { return t ? t->sb_count : 0; }

int unoterm_take_dirty(unoterm *t)
{
    int d;
    if (!t) return 0;
    d = t->dirty;
    t->dirty = 0;
    return d;
}

int unoterm_line_text(const unoterm *t, int y, char *out, int cap)
{
    const unoterm_cell *r;
    int x, n = 0, last = -1;
    if (!t || !out || cap <= 0) return 0;
    out[0] = 0;
    if (y < 0 || y >= t->rows) return 0;
    r = line_of(t, y);
    for (x = 0; x < t->cols; x++) if (r[x].ch != ' ' && r[x].ch) last = x;
    for (x = 0; x <= last && n < cap - 1; x++)
        out[n++] = (char)(r[x].ch < 0x80 && r[x].ch ? r[x].ch : '?');
    out[n] = 0;
    return n;
}

/* ---- input --------------------------------------------------------------- */
static int emit(char *out, int cap, int *n, const char *s)
{
    while (*s) { if (*n >= cap) return 0; out[(*n)++] = *s++; }
    return 1;
}

int unoterm_key(const unoterm *t, int key, int ctrl, int alt, char *out, int cap)
{
    int n = 0;
    /* The cursor-key prefix is CSI normally and SS3 in application mode.  Get
     * this wrong and the arrows work everywhere except inside the programs
     * that most need them (readline, vim, less) - which reads as "the arrow
     * keys are broken" rather than as a mode bug. */
    const char *ck = (t && t->app_cursor_keys) ? "\033O" : "\033[";

    if (!out || cap < 1) return 0;
    if (alt) { if (n < cap) out[n++] = 0x1b; }

    switch (key) {
    case UNOTERM_K_UP:    emit(out, cap, &n, ck); if (n < cap) out[n++] = 'A'; return n;
    case UNOTERM_K_DOWN:  emit(out, cap, &n, ck); if (n < cap) out[n++] = 'B'; return n;
    case UNOTERM_K_RIGHT: emit(out, cap, &n, ck); if (n < cap) out[n++] = 'C'; return n;
    case UNOTERM_K_LEFT:  emit(out, cap, &n, ck); if (n < cap) out[n++] = 'D'; return n;
    case UNOTERM_K_HOME:  emit(out, cap, &n, "\033[H");  return n;
    case UNOTERM_K_END:   emit(out, cap, &n, "\033[F");  return n;
    case UNOTERM_K_PGUP:  emit(out, cap, &n, "\033[5~"); return n;
    case UNOTERM_K_PGDN:  emit(out, cap, &n, "\033[6~"); return n;
    case UNOTERM_K_INS:   emit(out, cap, &n, "\033[2~"); return n;
    case UNOTERM_K_DEL:   emit(out, cap, &n, "\033[3~"); return n;
    case UNOTERM_K_F1:    emit(out, cap, &n, "\033OP");  return n;
    case UNOTERM_K_F2:    emit(out, cap, &n, "\033OQ");  return n;
    case UNOTERM_K_F3:    emit(out, cap, &n, "\033OR");  return n;
    case UNOTERM_K_F4:    emit(out, cap, &n, "\033OS");  return n;
    case UNOTERM_K_F5:    emit(out, cap, &n, "\033[15~"); return n;
    case UNOTERM_K_F6:    emit(out, cap, &n, "\033[17~"); return n;
    case UNOTERM_K_F7:    emit(out, cap, &n, "\033[18~"); return n;
    case UNOTERM_K_F8:    emit(out, cap, &n, "\033[19~"); return n;
    case UNOTERM_K_F9:    emit(out, cap, &n, "\033[20~"); return n;
    case UNOTERM_K_F10:   emit(out, cap, &n, "\033[21~"); return n;
    case UNOTERM_K_F11:   emit(out, cap, &n, "\033[23~"); return n;
    case UNOTERM_K_F12:   emit(out, cap, &n, "\033[24~"); return n;
    default: break;
    }

    if (ctrl && key >= 'a' && key <= 'z') { if (n < cap) out[n++] = (char)(key - 'a' + 1); return n; }
    if (ctrl && key >= 'A' && key <= 'Z') { if (n < cap) out[n++] = (char)(key - 'A' + 1); return n; }
    if (ctrl && key == ' ')               { if (n < cap) out[n++] = 0; return n; }

    /* Encode the code point as UTF-8: the far end asked for a UTF-8 terminal
     * in the pty request, so sending Latin-1 bytes for anything above 127
     * would be a different protocol. */
    if (key < 0x80) { if (n < cap) out[n++] = (char)key; }
    else if (key < 0x800) {
        if (n + 1 < cap) { out[n++] = (char)(0xC0 | (key >> 6));
                           out[n++] = (char)(0x80 | (key & 0x3F)); }
    } else if (key < 0x10000) {
        if (n + 2 < cap) { out[n++] = (char)(0xE0 | (key >> 12));
                           out[n++] = (char)(0x80 | ((key >> 6) & 0x3F));
                           out[n++] = (char)(0x80 | (key & 0x3F)); }
    }
    return n;
}

int unoterm_paste(const unoterm *t, const char *text, int len, char *out, int cap)
{
    int need, n = 0, i;
    int bracket = t && t->bracketed_paste;
    if (!text || len < 0) return 0;
    need = len + (bracket ? 12 : 0);
    if (!out || cap < need) return need;
    if (bracket) { const char *s = "\033[200~"; while (*s) out[n++] = *s++; }
    for (i = 0; i < len; i++) out[n++] = text[i];
    if (bracket) { const char *s = "\033[201~"; while (*s) out[n++] = *s++; }
    return n;
}

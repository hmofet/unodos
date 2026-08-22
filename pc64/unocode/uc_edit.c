/*
 * VENDORED FILE - DO NOT EDIT HERE.
 *
 * UnoCode is developed at https://github.com/hmofet/unocode-desktop, in its core/ directory.
 * An edit made here is lost at the next sync, and until then it silently
 * forks the editor away from the tree the desktop builds are cut from.
 *
 * Change it there; bring it back with pc64/tools/sync_unocode.py.
 * See pc64/UNOCODE-UPSTREAM.md.
 */
/* ===========================================================================
 * uc_edit.c - the editor view: the gutter, the text, the minimap, the find
 * widget and the suggestion list.
 *
 * ONE GEOMETRY FUNCTION.  Everything that has to agree about where a character
 * is - the painter, the mouse hit test, the caret, the selection runs, the
 * find highlights, the minimap slider - goes through vcol_of() / vcol_byte()
 * and the layout in ed_layout().  A tab is not one cell wide, and the classic
 * editor bug is a painter that expands tabs and a hit test that does not, so
 * a click lands one place and the caret appears in another.  There is exactly
 * one implementation here and both sides call it.
 *
 * THE SCOPE COLOUR CACHE.  Resolving a token scope to a colour is a walk over
 * the theme's rule list doing prefix compares; doing that per character per
 * frame would be the most expensive thing on screen.  Scopes are interned to
 * small ids by uc_lang.c, so the answer is memoised in an id-indexed table
 * that is dropped whenever the theme changes.
 * ======================================================================== */
#include "unocode.h"
#include "uc_lsp.h"

/* ---- metrics ---------------------------------------------------------------- */
static int g_mono = -1, g_px = 14, g_cw = 8, g_lh = 16, g_asc = 12;
static int g_uifont = -2, g_uipx = 13, g_uih = 16;

int uc_mono_slot(void) { return g_mono; }
int uc_font_px(void)   { return g_px; }
int uc_char_w(void)    { return g_cw; }
int uc_line_h(void)    { return g_lh; }
int uc_ui_h(void)      { return g_uih; }

void uc_metrics_init(void)
{
    g_mono = pc64_shell_font_mono();
    g_px = uc_cfg_int("editor.fontSize");
    if (g_px < 8) g_px = 8;
    if (g_px > 40) g_px = 40;
    g_cw = uno_font_text_w_styled(g_mono, g_px, 0, "M");
    if (g_cw < 3) g_cw = 8;
    g_lh = uno_font_height_px(g_mono, g_px);
    if (g_lh < 8) g_lh = g_px + 3;
    g_asc = uno_font_baseline_px(g_mono, g_px);
    if (g_asc <= 0 || g_asc > g_lh) g_asc = g_lh - 3;
    g_uipx = g_px - 1;
    if (g_uipx < 9) g_uipx = 9;
    g_uih = fb_text_h();
    if (g_uih < 10) g_uih = 12;
}

void uc_font_zoom(int delta)
{
    char num[16];
    int px = uc_cfg_int("editor.fontSize") + delta;
    if (px < 8) px = 8;
    if (px > 40) px = 40;
    uc_itoa(num, px);
    uc_cfg_set("editor.fontSize", num);
    uc_metrics_init();
}

/* ---- text helpers ----------------------------------------------------------- */
/* Draw n BYTES of s on the editor's grid, one cell per character.
 *
 * This used to hand the run to the proportional pen and rely on the mono face
 * advancing every glyph identically.  That held for ASCII by luck and stopped
 * holding the moment the run contained anything else: a two-byte character
 * measured as two cells, so the rest of the line - and the caret, selection
 * and hit test computed from column * g_cw - slid apart from the glyphs.
 * uno_font_draw_mono() places each character where the column arithmetic says
 * it goes, so the two cannot drift.
 *
 * Control bytes still become spaces.  A partial character at the end of the
 * run is dropped rather than drawn as U+FFFD: the run boundary is a colour
 * change inside a line, not the end of the text, and the next run will draw
 * those bytes with the rest of their character. */
int uc_mono_n(int x, int y, const char *s, int n, fb_px fg, int style)
{
    char buf[260];
    int i = 0, k = 0;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    while (i < n) {
        int cp, len = uc_u8_get(s + i, n - i, &cp);
        if (len <= 0) break;
        if (cp < 32) { buf[k++] = ' '; i += len; continue; }
        if (k + len >= (int)sizeof buf) break;
        memcpy(buf + k, s + i, (unsigned long)len);
        k += len; i += len;
    }
    buf[k] = 0;
    return uno_font_draw_mono(g_mono, g_px, style & 3, x, y, buf, g_cw, fg);
}

int uc_mono(int x, int y, const char *s, fb_px fg, int style)
{
    return uc_mono_n(x, y, s, (int)strlen(s), fg, style);
}

int uc_ui_text(int x, int y, const char *s, fb_px fg)
{
    (void)g_uifont;
    return fb_text(x, y, s, fg, -1);
}

int uc_ui_text_w(const char *s) { return fb_text_w(s); }

/* Draw `s`, shortened with an ellipsis if it will not fit in `maxw`.
 *
 * Clipping alone cuts the last glyph in half and reads as a rendering fault;
 * an ellipsis reads as "there is more", which is what is true.  Used by the
 * lists whose rows are narrower than their content - the Explorer, the
 * Extensions view - where the alternative is a wall of half-letters. */
int uc_ui_text_fit(int x, int y, const char *s, int maxw, fb_px fg)
{
    char buf[160];
    int n;
    if (maxw <= 0) return x;
    if (uc_ui_text_w(s) <= maxw) return uc_ui_text(x, y, s, fg);
    uc_scpy(buf, s, sizeof buf);
    n = (int)strlen(buf);
    while (n > 1) {
        buf[n - 1] = 0;
        buf[n] = 0;
        /* three ASCII dots, not U+2026: the shipped faces do not all carry the
         * ellipsis glyph, and a truncation mark that renders as a blank is
         * indistinguishable from text that simply stopped. */
        uc_scat(buf, "...", sizeof buf);
        if (uc_ui_text_w(buf) <= maxw) break;
        buf[n - 1] = 0;
        n--;
    }
    return uc_ui_text(x, y, buf, fg);
}

/* ---- scope colour cache ------------------------------------------------------ */
#define SC_CACHE 256
static fb_px         sc_col[SC_CACHE];
static unsigned char sc_style[SC_CACHE], sc_have[SC_CACHE];
static UcTheme      *sc_theme;

static void sc_check(void)
{
    if (sc_theme != uc_theme_active()) {
        sc_theme = uc_theme_active();
        memset(sc_have, 0, sizeof sc_have);
    }
}

static fb_px scope_color(int id, int *style)
{
    if (id < 0 || id >= SC_CACHE) { if (style) *style = 0; return uc_col(UC_C_EDITOR_FG); }
    if (!sc_have[id]) {
        int st = 0;
        sc_col[id] = uc_tok_color(uc_scope_name(id), &st);
        sc_style[id] = (unsigned char)st;
        sc_have[id] = 1;
    }
    if (style) *style = sc_style[id];
    return sc_col[id];
}

/* ---- visual columns (tabs) ---------------------------------------------------- */
/* The visual column of byte offset `byte`.  A CHARACTER is one cell (two for a
 * wide one, none for a combining mark), not a byte: count bytes here and every
 * accented letter shifts the caret, the selection and the find highlight one
 * cell further right than the glyph it is meant to sit on. */
static int vcol_of(const char *s, int n, int byte, int ts)
{
    int i = 0, v = 0;
    if (byte > n) byte = n;
    while (i < byte) {
        int cp, len;
        if (s[i] == '\t') { v += ts - (v % ts); i++; continue; }
        len = uc_u8_get(s + i, n - i, &cp);
        if (len <= 0) break;
        v += uc_cp_width(cp);
        i += len;
    }
    return v;
}

/* the width in CELLS of n bytes of text, for the paint loop: a run's byte
 * count and its column count are the same number only in ASCII */
static int run_cells(const char *s, int n)
{
    int i = 0, v = 0;
    while (i < n) {
        int cp, len = uc_u8_get(s + i, n - i, &cp);
        if (len <= 0) break;
        v += uc_cp_width(cp);
        i += len;
    }
    return v;
}

/* the byte index whose cell contains visual column `want`; always a character
 * boundary, so a caret placed from it can never land inside a sequence */
static int vcol_byte(const char *s, int n, int want, int ts)
{
    int i = 0, v = 0;
    while (i < n) {
        int cp, len, w;
        if (s[i] == '\t') {
            w = ts - (v % ts);
            if (v + w > want) return i;
            v += w; i++;
            continue;
        }
        len = uc_u8_get(s + i, n - i, &cp);
        if (len <= 0) break;
        w = uc_cp_width(cp);
        if (w && v + w > want) return i;
        v += w; i += len;
    }
    return n;
}

/* ---- layout ------------------------------------------------------------------- */
typedef struct { UcRect gutter, text, minimap, bar; int digits; } EdLayout;

#define UC_MINIMAP_W 74
#define UC_BAR_W     12

static EdLayout ed_layout(UcRect r, UcDoc *d)
{
    EdLayout L;
    int nl = d ? uc_line_count(d) : 1, dg = 1, t = nl;
    int gw = 0, mw = 0;
    while (t >= 10) { t /= 10; dg++; }
    if (dg < 3) dg = 3;
    L.digits = dg;
    if (strcmp(uc_cfg_str("editor.lineNumbers"), "off"))
        gw = dg * g_cw + 18;
    else
        gw = 12;
    if (uc_cfg_bool("editor.minimap.enabled") && r.w > 420) mw = UC_MINIMAP_W;
    L.gutter  = (UcRect){ r.x, r.y, gw, r.h };
    L.bar     = (UcRect){ r.x + r.w - UC_BAR_W, r.y, UC_BAR_W, r.h };
    L.minimap = (UcRect){ r.x + r.w - UC_BAR_W - mw, r.y, mw, r.h };
    L.text    = (UcRect){ r.x + gw, r.y, r.w - gw - mw - UC_BAR_W, r.h };
    if (L.text.w < 40) { L.text.w = r.w - gw; L.minimap.w = 0; L.bar.w = 0; }
    return L;
}

int uc_edit_rows(UcRect r) { int n = r.h / g_lh; return n > 0 ? n : 1; }

/* ---- find state (the widget lives over the editor) ---------------------------- */
static int   find_on, find_replace_mode, find_focus;   /* focus 0=find 1=replace */
static char  find_str[120], repl_str[120];
static int   find_len, repl_len;
static int   find_case, find_word, find_regex;
static int   find_total, find_index;
static UcRx *find_rx;
static int   find_rx_bad;

/* ---- suggestions -------------------------------------------------------------- */
#define SUG_MAX 64
typedef struct {
    /* Wider than the word-scraper needed (UCD-24).  A server's label is a
     * whole overload name and its detail is a full signature - `const
     * std::vector<T> &` is already most of the old 40 bytes - and a truncated
     * completion is worse than none, because it inserts. */
    char label[64];
    char detail[96];
    char insert[128];
    int  kind;
    int  score;
} UcSuggest;
static UcSuggest sug[SUG_MAX];
static int  sug_n, sug_sel, sug_on, sug_scroll;
static int  sug_word_start;
static char sug_prefix[48];
/* 1 when the list on screen came from a language server rather than from the
 * word scraper.  Read by uc_suggest_from_server(), which is how a test can
 * tell a real completion list from a plausible-looking local one. */
static int  sug_from_server;
/* defined below with the rest of the language-server path (UCD-24) */
static void sug_ask_server(UcDoc *d, int caret);

/* ---- hover (UCD-25) ------------------------------------------------------------
 * A popup with the server's answer about the symbol under the pointer.
 *
 * IT IS DRIVEN BY A DWELL, NOT BY THE MOVE.  Asking on every mouse-move event
 * would put a request on the wire for every pixel the pointer crosses, and
 * would show a box the instant the pointer passed over a name on its way
 * somewhere else - which is the behaviour that makes people turn hovers off.
 * The pointer has to stop for UC_HOVER_MS over the same word.
 *
 * THE CONTENT ARRIVES AS MARKDOWN.  Servers send `contents` as a MarkupContent,
 * a MarkedString, or an array of either, and clangd sends fenced code blocks
 * inside it.  This strips the fences and the emphasis rather than rendering
 * them: a monospace box showing ```cpp on its own line is worse than plain
 * text, and a markdown renderer is not what this task is.
 * ======================================================================== */
#define UC_HOVER_MS   450
#define UC_HOVER_MAX  1400
#define UC_HOVER_ROWS 12
static int  hov_on, hov_off, hov_x, hov_y, hov_gen, hov_pending;
static unsigned long hov_dwell_at;
static int  hov_want_x, hov_want_y;
static char hov_text[UC_HOVER_MAX];
static void hov_pointer(UcRect r, UcDoc *d, int px, int py);

/* ---- painting ----------------------------------------------------------------- */
static void draw_hscroll_clip(UcRect r) { fb_set_clip(r.x, r.y, r.w, r.h); }

static void caret_col_of(UcDoc *d, int off, int *line, int *vcol)
{
    int ln = uc_line_of(d, off), s = uc_line_start(d, ln), e = uc_line_end(d, ln);
    *line = ln;
    *vcol = vcol_of(d->text + s, e - s, off - s, uc_doc_tabsize(d));
}

void uc_edit_reveal(UcRect r, UcDoc *d)
{
    EdLayout L;
    int line, vcol, rows, cols;
    if (!d) return;
    L = ed_layout(r, d);
    rows = uc_edit_rows(r);
    cols = L.text.w / g_cw;
    if (cols < 1) cols = 1;
    caret_col_of(d, d->cur[d->ncur - 1].caret, &line, &vcol);
    if (line < d->scroll_line) d->scroll_line = line;
    if (line >= d->scroll_line + rows) d->scroll_line = line - rows + 1;
    if (vcol < d->scroll_col) d->scroll_col = vcol;
    if (vcol >= d->scroll_col + cols) d->scroll_col = vcol - cols + 1;
    if (d->scroll_line < 0) d->scroll_line = 0;
    if (d->scroll_col < 0) d->scroll_col = 0;
}

static int sel_on_line(UcDoc *d, int line, int *a, int *b)
{
    int i, s = uc_line_start(d, line), e = uc_line_end(d, line), got = 0;
    int lo = 0, hi = 0;
    for (i = 0; i < d->ncur; i++) {
        int ca = d->cur[i].anchor, cb = d->cur[i].caret, t;
        if (ca == cb) continue;
        if (ca > cb) { t = ca; ca = cb; cb = t; }
        if (cb < s || ca > e) continue;
        if (ca < s) ca = s;
        if (cb > e) cb = e + 1;              /* include the line break */
        if (!got) { lo = ca; hi = cb; got = 1; }
        else { if (ca < lo) lo = ca; if (cb > hi) hi = cb; }
    }
    *a = lo; *b = hi;
    return got;
}

/* A wavy underline, built from the primitives that exist (UCD-23).
 *
 * The framebuffer has no wave and no dotted line, so this is the two-pixel saw
 * every editor draws: a 4-pixel period, one pixel up and one down.  It is not
 * decoration - it is what distinguishes a diagnostic from the straight
 * underline a `markup.underline` scope already draws two pixels lower, and a
 * squiggle drawn straight would read as a link. */
static void squiggle(int x, int y, int w, fb_px col)   /* like fb_hline */
{
    int i;
    for (i = 0; i < w; i += 4) {
        fb_fill_rect(x + i,     y,     2, 1, col);
        fb_fill_rect(x + i + 2, y - 1, 2, 1, col);
    }
}

static void draw_minimap(UcRect m, UcDoc *d, int first_row, int rows)
{
    int nl = uc_line_count(d), i, step, y, shown;
    if (m.w <= 0) return;
    fb_fill_rect(m.x, m.y, m.w, m.h, uc_col(UC_C_EDITOR_BG));
    shown = m.h / 2;
    step = 1;
    if (nl > shown && shown > 0) step = (nl + shown - 1) / shown;
    y = m.y;
    for (i = 0; i < nl && y < m.y + m.h; i += step, y += 2) {
        int s = uc_line_start(d, i), e = uc_line_end(d, i), n = e - s, k;
        int x = m.x + 1;
        for (k = 0; k < n && x < m.x + m.w - 1; k++) {
            char c = d->text[s + k];
            if (c == ' ' || c == '\t') { x += (c == '\t') ? 4 : 1; continue; }
            fb_fill_rect(x, y, 1, 1, uc_blend(uc_col(UC_C_EDITOR_FG),
                                              uc_col(UC_C_EDITOR_BG), 150));
            x++;
        }
    }
    /* the viewport slider */
    if (nl > 0) {
        int top = m.y + (first_row / step) * 2;
        int hgt = (rows / step) * 2;
        if (hgt < 6) hgt = 6;
        if (top + hgt > m.y + m.h) top = m.y + m.h - hgt;
        if (top < m.y) top = m.y;
        fb_blend_rect(m.x, top, m.w, hgt, uc_col(UC_C_EDITOR_FG), 28);
    }

    /* Problem marks, OVER the slider (UCD-23).
     *
     * Under it looks tidier and does not work: the slider is a 28/255 blend,
     * so it does not hide a mark but it does shift its colour - and in a short
     * file the slider covers the whole minimap, which is exactly when every
     * mark is inside it.  Over it, a mark is the colour it claims to be.
     *
     * Left edge, 4 px wide.  The minimap's job is to show the SHAPE of the
     * file; a mark wide enough to cover that shape hides the thing it is
     * pointing into.
     */
    {
        /* Over the PROBLEMS, not over the lines.  A file has thousands of
         * lines and at most a hundred problems, and asking every line whether
         * it has one is that product, every frame. */
        int i2, np = uc_problems_total();
        for (i2 = 0; i2 < np; i2++) {
            UcProblem *p = uc_problem_at(i2);
            int my;
            if (!uc_problem_in_doc(p, d)) continue;
            my = m.y + ((p->line - 1) / step) * 2;
            if (my < m.y || my >= m.y + m.h) continue;
            fb_fill_rect(m.x, my, 4, 2,
                         uc_col(p->sev == UC_SEV_ERROR ? UC_C_ERROR_FG :
                                p->sev == UC_SEV_WARN  ? UC_C_WARN_FG
                                                       : UC_C_INFO_FG));
        }
    }

}

static void draw_vscroll(UcRect b, UcDoc *d, int first_row, int rows)
{
    int nl = uc_line_count(d);
    if (b.w <= 0) return;
    fb_fill_rect(b.x, b.y, b.w, b.h, uc_col(UC_C_EDITOR_BG));

    if (nl > rows) {
        int th = b.h * rows / nl;
        int ty;
        if (th < 20) th = 20;
        ty = b.y + (b.h - th) * first_row / (nl - rows);
        fb_fill_rect(b.x + 2, ty, b.w - 4, th, uc_col(UC_C_SCROLL_SLIDER));
    }

    /* The overview ruler (UCD-23): a mark per problem, over the WHOLE track,
     * and painted OVER the slider.
     *
     * Two deliberate choices.  The mapping is not the slider's: the slider's
     * track is shortened by its own height so a slider at the bottom stays
     * fully visible, and a mark placed with that formula sits above the line
     * it points at by up to a slider's worth - which is exactly the distance
     * that makes clicking one land somewhere else.  And it is drawn last,
     * because a two-pixel mark hidden under the slider is a mark that
     * disappears whenever you scroll to the problem it is pointing at.
     *
     * It also runs when the file fits on screen and there is no slider at all:
     * a short file still has problems worth marking. */
    {
        int i, np = uc_problems_total();
        for (i = 0; i < np && nl > 0; i++) {
            UcProblem *p = uc_problem_at(i);
            int y;
            if (!uc_problem_in_doc(p, d)) continue;
            y = b.y + b.h * (p->line - 1) / nl;
            if (y < b.y) y = b.y;
            if (y > b.y + b.h - 2) y = b.y + b.h - 2;
            fb_fill_rect(b.x + 1, y, b.w - 2, 2,
                         uc_col(p->sev == UC_SEV_ERROR ? UC_C_ERROR_FG :
                                p->sev == UC_SEV_WARN  ? UC_C_WARN_FG
                                                       : UC_C_INFO_FG));
        }
    }
}

void uc_edit_draw(UcRect r, UcDoc *d, int focused)
{
    EdLayout L;
    int rows, i, ts, caret_line, caret_vcol, blink;
    fb_px bg = uc_col(UC_C_EDITOR_BG);
    static short scopes[UC_HL_MAXLINE];
    int show_ws = 0, guides, ruler;
    const char *wsmode;

    sc_check();
    fb_fill_rect(r.x, r.y, r.w, r.h, bg);
    if (!d) {
        /* an empty editor group: VS Code shows its keyboard cheat sheet here */
        static const char *const hint[] = {
            "Show All Commands      Ctrl+Shift+P",
            "Go to File             Ctrl+P",
            "Find in Files          Ctrl+Shift+F",
            "Toggle Terminal        Ctrl+`",
            "Open Settings          Ctrl+,"
        };
        int y = r.y + r.h / 2 - 3 * g_uih, k;
        fb_px dim = uc_blend(uc_col(UC_C_EDITOR_FG), bg, 90);
        for (k = 0; k < 5; k++)
            uc_ui_text(r.x + r.w / 2 - 130, y + k * (g_uih + 6), hint[k], dim);
        return;
    }

    L = ed_layout(r, d);
    rows = uc_edit_rows(r);
    ts = uc_doc_tabsize(d);
    caret_col_of(d, d->cur[d->ncur - 1].caret, &caret_line, &caret_vcol);
    blink = !uc_cfg_bool("editor.cursorBlinking") || ((uno_dbg_uptime_ms() / 530) & 1);
    wsmode = uc_cfg_str("editor.renderWhitespace");
    show_ws = strcmp(wsmode, "none") != 0;
    guides = uc_cfg_bool("editor.renderIndentGuides");
    ruler = uc_cfg_int("editor.rulers");

    /* the ruler sits under the text */
    if (ruler > 0) {
        int x = L.text.x + (ruler - d->scroll_col) * g_cw;
        if (x > L.text.x && x < L.text.x + L.text.w)
            fb_vline(x, L.text.y, L.text.h, uc_col(UC_C_RULER));
    }

    for (i = 0; i < rows; i++) {
        int line = d->scroll_line + i;
        int y = r.y + i * g_lh;
        int s, e, n, sa, sb, cx;
        int state, out;
        if (line >= uc_line_count(d)) break;
        s = uc_line_start(d, line);
        e = uc_line_end(d, line);
        n = e - s;

        /* current-line highlight */
        if (line == caret_line && uc_cfg_bool("editor.renderLineHighlight") && !uc_has_selection(d))
            fb_fill_rect(L.text.x, y, L.text.w, g_lh, uc_col(UC_C_LINE_HL));

        /* gutter: line number + change bar */
        if (L.gutter.w > 12) {
            char num[16];
            int rel = !strcmp(uc_cfg_str("editor.lineNumbers"), "relative");
            int shown = rel && line != caret_line ? (line > caret_line ? line - caret_line
                                                                      : caret_line - line)
                                                  : line + 1;
            int w;
            uc_itoa(num, shown);
            w = (int)strlen(num) * g_cw;
            uc_mono(L.gutter.x + L.gutter.w - 12 - w, y,
                    num, line == caret_line ? uc_col(UC_C_LINENO_ACTIVE)
                                            : uc_col(UC_C_LINENO), 0);
        }
        {
            int ch = uc_line_changed(d, line);
            if (ch) fb_fill_rect(L.gutter.x + L.gutter.w - 5, y, 3, g_lh,
                                 ch == 2 ? uc_col(UC_C_GUTTER_ADDED)
                                         : uc_col(UC_C_GUTTER_MODIFIED));
        }

        draw_hscroll_clip(L.text);

        /* selection */
        if (sel_on_line(d, line, &sa, &sb)) {
            int va = vcol_of(d->text + s, n, sa - s, ts);
            int vb = vcol_of(d->text + s, n, sb - s > n ? n : sb - s, ts);
            int x0 = L.text.x + (va - d->scroll_col) * g_cw;
            int x1 = L.text.x + (vb - d->scroll_col) * g_cw;
            if (sb > e) x1 += g_cw / 2;                 /* the wrapped newline */
            if (x1 > x0) fb_fill_rect(x0, y, x1 - x0, g_lh, uc_col(UC_C_SELECTION));
        }

        /* find matches */
        {
            int st[24], en[24], k, nh;
            nh = uc_find_line_hits(d, line, st, en, 24);
            for (k = 0; k < nh; k++) {
                int va = vcol_of(d->text + s, n, st[k], ts);
                int vb = vcol_of(d->text + s, n, en[k], ts);
                int x0 = L.text.x + (va - d->scroll_col) * g_cw;
                int x1 = L.text.x + (vb - d->scroll_col) * g_cw;
                if (x1 > x0) fb_fill_rect(x0, y, x1 - x0, g_lh, uc_col(UC_C_FIND_MATCH_HL));
            }
        }

        /* indent guides */
        if (guides) {
            int v = 0, k;
            for (k = 0; k < n && (d->text[s+k] == ' ' || d->text[s+k] == '\t'); k++)
                v += (d->text[s+k] == '\t') ? ts - (v % ts) : 1;
            for (k = ts; k <= v; k += ts) {
                int x = L.text.x + (k - ts - d->scroll_col) * g_cw;
                if (x >= L.text.x) fb_vline(x, y, g_lh, uc_col(UC_C_INDENT_GUIDE));
            }
        }

        /* text, one run per colour */
        state = uc_line_state(d, line);
        {
            int hn = n > UC_HL_MAXLINE ? UC_HL_MAXLINE : n;
            int coloured = uc_tokenize(d->lang, d->text + s, hn, state, scopes, &out);
            int k = 0, v = 0, vw;
            if (!coloured) for (k = 0; k < hn; k++) scopes[k] = 0;
            k = 0;
            while (k < n) {
                int id = (k < hn) ? scopes[k] : 0;
                int run = k, style = 0;
                fb_px col;
                int x;
                if (d->text[s + k] == '\t') {
                    int w = ts - (v % ts);
                    if (show_ws) {
                        x = L.text.x + (v - d->scroll_col) * g_cw;
                        if (x >= L.text.x - g_cw)
                            uc_mono_n(x, y, "\xc2\xbb", 2, uc_col(UC_C_WHITESPACE), 0);
                    }
                    v += w; k++;
                    continue;
                }
                if (d->text[s + k] == ' ') {
                    if (show_ws && (strcmp(wsmode, "all") == 0 ||
                                    k == 0 || k == n - 1 ||
                                    d->text[s+k-1] == ' ' || d->text[s+k+1] == ' ')) {
                        x = L.text.x + (v - d->scroll_col) * g_cw;
                        fb_fill_rect(x + g_cw/2 - 1, y + g_lh/2, 2, 2, uc_col(UC_C_WHITESPACE));
                    }
                    v++; k++;
                    continue;
                }
                while (k < n && d->text[s + k] != '\t' && d->text[s + k] != ' ' &&
                       ((k < hn) ? scopes[k] : 0) == id) k++;
                /* The tokenizer scopes BYTES, so a run may end in the middle
                 * of a character.  Carry the rest of it into this run: split
                 * it and both halves decode as U+FFFD, drawing two wrong
                 * glyphs where one right one belongs. */
                while (k < n && ((unsigned char)d->text[s + k] & 0xC0) == 0x80) k++;
                col = scope_color(id, &style);
                x = L.text.x + (v - d->scroll_col) * g_cw;
                /* the run's width in CELLS, which is not its length in bytes */
                vw = run_cells(d->text + s + run, k - run);
                if (x + vw * g_cw >= L.text.x && x < L.text.x + L.text.w) {
                    uc_mono_n(x, y, d->text + s + run, k - run, col, style);
                    if (style & UC_FS_UNDERLINE)
                        fb_hline(x, y + g_lh - 2, vw * g_cw, col);
                }
                v += vw;
                if (v - d->scroll_col > L.text.w / g_cw + 2) break;
            }
        }

        /* Squiggles, after the glyphs and inside the same clip (UCD-23).
         *
         * A DIAGNOSTIC WITH NO RANGE STILL HAS TO UNDERLINE SOMETHING.  A build
         * prints `file:12:5:` and nothing more, so end_col is 0 and there is no
         * width to draw; underlining a single cell is a mark nobody can see and
         * underlining the line is a claim the compiler did not make.  The word
         * at the column is the honest middle - it is what the diagnostic is
         * almost always about, and it is what VS Code shows for the same input.
         *
         * A range that ENDS ON A LATER LINE is clamped to this line's end
         * rather than wrapped: the run is drawn per line and a multi-line
         * range is several runs, not one. */
        {
            UcProblem *probs[8];
            int np = uc_problems_on_line(d, line, probs, 8), pi;
            for (pi = 0; pi < np; pi++) {
                UcProblem *p = probs[pi];
                int a = uc_offset_of(d, line, p->col > 0 ? p->col - 1 : 0);
                int b = (p->end_line - 1 > line) ? e
                      : (p->end_col > 0) ? uc_offset_of(d, line, p->end_col - 1)
                      : uc_word_end(d, a);
                int va, vb, x0, x1;
                if (b <= a) b = uc_word_end(d, a);
                if (b <= a) b = (a < e) ? a + 1 : e;
                if (a < s) a = s;
                if (b > e) b = e;
                va = vcol_of(d->text + s, n, a - s, ts);
                vb = vcol_of(d->text + s, n, b - s, ts);
                x0 = L.text.x + (va - d->scroll_col) * g_cw;
                x1 = L.text.x + (vb - d->scroll_col) * g_cw;
                if (x1 > x0)
                    squiggle(x0, y + g_lh - 2, x1 - x0,
                             uc_col(p->sev == UC_SEV_ERROR ? UC_C_ERROR_FG :
                                    p->sev == UC_SEV_WARN  ? UC_C_WARN_FG
                                                           : UC_C_INFO_FG));
            }
        }

        /* bracket match */
        if (uc_cfg_bool("editor.matchBrackets") && line == caret_line) {
            int off = d->cur[d->ncur-1].caret;
            int m = uc_bracket_match(d, off > 0 ? off - 1 : off);
            int base = off > 0 ? off - 1 : off;
            if (m < 0) { m = uc_bracket_match(d, off); base = off; }
            if (m >= 0) {
                int pairs[2];
                int p;
                pairs[0] = base; pairs[1] = m;
                for (p = 0; p < 2; p++) {
                    int pl = uc_line_of(d, pairs[p]);
                    if (pl < d->scroll_line || pl >= d->scroll_line + rows) continue;
                    {
                        int ps = uc_line_start(d, pl), pe = uc_line_end(d, pl);
                        int pv = vcol_of(d->text + ps, pe - ps, pairs[p] - ps, ts);
                        int px = L.text.x + (pv - d->scroll_col) * g_cw;
                        int py = r.y + (pl - d->scroll_line) * g_lh;
                        fb_frame_rect(px, py, g_cw, g_lh, uc_col(UC_C_BRACKET_MATCH));
                    }
                }
            }
        }

        /* carets */
        if (focused && blink) {
            int c;
            for (c = 0; c < d->ncur; c++) {
                int cl, cv;
                caret_col_of(d, d->cur[c].caret, &cl, &cv);
                if (cl != line) continue;
                cx = L.text.x + (cv - d->scroll_col) * g_cw;
                if (cx >= L.text.x - 1 && cx < L.text.x + L.text.w)
                    fb_fill_rect(cx, y, 2, g_lh, uc_col(UC_C_CURSOR));
            }
        }
        fb_reset_clip();
    }

    draw_minimap(L.minimap, d, d->scroll_line, rows);
    draw_vscroll(L.bar, d, d->scroll_line, rows);

    /* The overlays belong to the editor being TYPED IN, not to every editor
     * on screen.  With two groups (UCD-18) this painted the suggestion list
     * and the find box into both panes, one of which nobody was typing in. */
    if (focused) {
        if (find_on) uc_find_draw(r);
        if (sug_on)  uc_suggest_draw(r, d);
    }
}

/* ---- hit testing --------------------------------------------------------------- */
/* Is (x,y) over the TEXT, as opposed to the gutter, the minimap or the scroll
 * bar?  Only the text takes an I-beam, and only this file knows where it is -
 * ed_layout() is the one place the editor's sub-rects are computed, and a host
 * re-deriving them would drift the first time they changed. */
int uc_edit_over_text(UcRect r, int x, int y)
{
    EdLayout L = ed_layout(r, uc_doc_active());
    return x >= L.text.x && x < L.text.x + L.text.w &&
           y >= L.text.y && y < L.text.y + L.text.h;
}

static int offset_at(UcRect r, UcDoc *d, int px, int py)
{
    EdLayout L = ed_layout(r, d);
    int line = d->scroll_line + (py - r.y) / g_lh;
    int s, e, n, want;
    if (line < 0) line = 0;
    if (line >= uc_line_count(d)) line = uc_line_count(d) - 1;
    s = uc_line_start(d, line);
    e = uc_line_end(d, line);
    n = e - s;
    want = (px - L.text.x + g_cw / 2) / g_cw + d->scroll_col;
    if (want < 0) want = 0;
    return s + vcol_byte(d->text + s, n, want, uc_doc_tabsize(d));
}

/* ---- input --------------------------------------------------------------------- */
static unsigned long last_click_ms;
static int last_click_off, click_run;
static int g_box_anchor;               /* where an Alt+Shift column drag began */

int uc_edit_event(UcRect r, UcDoc *d, const unoui_event *e)
{
    EdLayout L;
    if (!d) return 0;
    L = ed_layout(r, d);
    if (e->kind == UI_EV_WHEEL) {
        d->scroll_line += e->wheel * 3;
        if (d->scroll_line > uc_line_count(d) - 1) d->scroll_line = uc_line_count(d) - 1;
        if (d->scroll_line < 0) d->scroll_line = 0;
        return 1;
    }
    if (e->kind == UI_EV_MOUSE_DOWN) {
        unsigned long now = uno_dbg_uptime_ms();
        int off;
        /* the scrollbar and the minimap jump; the text places the caret */
        if (L.bar.w && e->x >= L.bar.x) {
            int nl = uc_line_count(d), rows = uc_edit_rows(r);
            if (nl > rows) {
                d->scroll_line = (e->y - r.y) * (nl - rows) / (r.h ? r.h : 1);
                if (d->scroll_line < 0) d->scroll_line = 0;
                if (d->scroll_line > nl - rows) d->scroll_line = nl - rows;
            }
            UC.drag = UC_DRAG_VSCROLL;
            return 1;
        }
        if (L.minimap.w && e->x >= L.minimap.x) {
            int nl = uc_line_count(d), rows = uc_edit_rows(r);
            int step = 1, shown = r.h / 2;
            if (nl > shown && shown > 0) step = (nl + shown - 1) / shown;
            d->scroll_line = ((e->y - r.y) / 2) * step - rows / 2;
            if (d->scroll_line < 0) d->scroll_line = 0;
            if (d->scroll_line > nl - 1) d->scroll_line = nl - 1;
            UC.drag = UC_DRAG_MINIMAP;
            return 1;
        }
        off = offset_at(r, d, e->x, e->y);
        if (now - last_click_ms < 450 && off == last_click_off) click_run++;
        else click_run = 1;
        last_click_ms = now;
        last_click_off = off;
        if (click_run >= 3) {
            uc_move_to(d, off, 0);
            uc_select_line(d);
        } else if (click_run == 2) {
            uc_move_to(d, off, 0);
            uc_select_word(d);
        } else if ((e->mods & UI_MOD_ALT) && (e->mods & UI_MOD_SHIFT)) {
            /* Alt+Shift starts a COLUMN selection: one cursor per line
             * between the anchor and the pointer, each selecting the same
             * column span (UCD-16) */
            uc_move_to(d, off, 0);
            g_box_anchor = off;
            UC.drag = UC_DRAG_BOX;
        } else if (e->mods & UI_MOD_ALT) {
            uc_add_cursor(d, off);
        } else {
            uc_move_to(d, off, (e->mods & UI_MOD_SHIFT) != 0);
            UC.drag = UC_DRAG_TEXT;
        }
        uc_suggest_close();
        return 1;
    }
    if (e->kind == UI_EV_MOUSE_MOVE && UC.drag == UC_DRAG_TEXT) {
        int off = offset_at(r, d, e->x, e->y);
        d->cur[d->ncur - 1].caret = off;
        uc_edit_reveal(r, d);
        return 1;
    }
    if (e->kind == UI_EV_MOUSE_MOVE && UC.drag == UC_DRAG_BOX) {
        /* Rebuilt from scratch every motion, rather than grown: dragging back
         * up has to REMOVE the cursors the way down created, and a box that
         * only ever added them would leave a trail behind the pointer. */
        int off = offset_at(r, d, e->x, e->y);
        int l0 = uc_line_of(d, g_box_anchor), c0 = uc_col_of(d, g_box_anchor);
        int l1 = uc_line_of(d, off), c1 = uc_col_of(d, off);
        int step = l1 >= l0 ? 1 : -1, L;
        uc_clear_extra_cursors(d);
        d->ncur = 0;
        for (L = l0; ; L += step) {
            int a = uc_offset_of(d, L, c0), b = uc_offset_of(d, L, c1);
            if (d->ncur >= UC_CURSORS_MAX) break;
            uc_add_cursor_sel(d, a, b);
            if (L == l1) break;
        }
        if (!d->ncur) uc_add_cursor_sel(d, g_box_anchor, off);
        uc_edit_reveal(r, d);
        uc_repaint();
        return 1;
    }
    if (e->kind == UI_EV_MOUSE_MOVE && UC.drag == UC_DRAG_VSCROLL) {
        int nl = uc_line_count(d), rows = uc_edit_rows(r);
        if (nl > rows) {
            d->scroll_line = (e->y - r.y) * (nl - rows) / (r.h ? r.h : 1);
            if (d->scroll_line < 0) d->scroll_line = 0;
            if (d->scroll_line > nl - rows) d->scroll_line = nl - rows;
        }
        return 1;
    }
    if (e->kind == UI_EV_MOUSE_UP) { UC.drag = UC_DRAG_NONE; return 1; }
    /* A plain move over the text, no button held: start (or restart) the hover
     * dwell.  Returns 0 so this is not treated as handling the event - the
     * pointer moving is not an interaction with the editor, and claiming it
     * would stop the cursor-shape code below from seeing it. */
    if (e->kind == UI_EV_MOUSE_MOVE && UC.drag == UC_DRAG_NONE) {
        if (e->x >= L.text.x && e->x < L.text.x + L.text.w &&
            e->y >= L.text.y && e->y < L.text.y + L.text.h)
            hov_pointer(r, d, e->x, e->y);
        else uc_hover_close();
    }
    return 0;
}

/* `ch` is a CODEPOINT, not a byte.  Above ASCII it is encoded into the buffer
 * as UTF-8 and none of the ASCII-shaped behaviour below applies: there is no
 * bracket to close, no quote to step over, and a suggestion list keyed on
 * identifier characters has nothing to say about an em dash. */
int uc_edit_char(UcDoc *d, int ch)
{
    char pair[5];
    int close;
    if (!d || ch < 32 || ch == 127) return 0;
    if (ch > 126) {
        int n = uc_u8_put(ch, pair);
        if (sug_on) uc_suggest_close();
        uc_insert(d, pair, n);
        uc_api_fire_change(d);
        return 1;
    }
    if (sug_on && !uc_is_word(ch) && ch != '.') uc_suggest_close();

    /* typing the closing half of a pair the editor inserted just steps over it */
    if (uc_cfg_bool("editor.autoClosingBrackets") && !uc_has_selection(d)) {
        int c = d->cur[0].caret;
        if ((ch == ')' || ch == ']' || ch == '}' || ch == '"' || ch == '\'') &&
            c < d->len && d->text[c] == ch) {
            uc_move(d, 1, 0, 0, 0);
            return 1;
        }
    }
    close = 0;
    if (uc_cfg_bool("editor.autoClosingBrackets")) {
        switch (ch) {
        case '(':  close = ')';  break;
        case '[':  close = ']';  break;
        case '{':  close = '}';  break;
        case '"':  close = '"';  break;
        case '\'': close = '\''; break;
        default:   close = 0;    break;
        }
    }

    if (close && !uc_has_selection(d)) {
        int c = d->cur[0].caret;
        /* do not auto-close in the middle of a word: `foo(` yes, `fo(o` no */
        if (c >= d->len || !uc_is_word((unsigned char)d->text[c])) {
            pair[0] = (char)ch; pair[1] = (char)close; pair[2] = 0;
            uc_insert(d, pair, 2);
            uc_move(d, -1, 0, 0, 0);
            if (uc_cfg_bool("editor.quickSuggestions")) uc_suggest_retrigger(d);
            return 1;
        }
    }
    pair[0] = (char)ch; pair[1] = 0;
    uc_insert(d, pair, 1);
    if (uc_cfg_bool("editor.quickSuggestions") && (uc_is_word(ch) || ch == '.'))
        uc_suggest_retrigger(d);
    uc_api_fire_change(d);
    return 1;
}

int uc_edit_key(UcDoc *d, int key, int mods)
{
    int shift = (mods & UI_MOD_SHIFT) != 0;
    int ctrl  = (mods & UI_MOD_CTRL) != 0;
    if (!d) return 0;
    if (sug_on && uc_suggest_key(key, mods)) return 1;
    switch (key) {
    case UI_KEY_LEFT:  uc_move(d, -1, 0, shift, ctrl); return 1;
    case UI_KEY_RIGHT: uc_move(d,  1, 0, shift, ctrl); return 1;
    case UI_KEY_UP:
        if (ctrl && (mods & UI_MOD_ALT)) { uc_add_cursor_line(d, -1); return 1; }
        uc_move(d, 0, -1, shift, 0); return 1;
    case UI_KEY_DOWN:
        if (ctrl && (mods & UI_MOD_ALT)) { uc_add_cursor_line(d, 1); return 1; }
        uc_move(d, 0, 1, shift, 0); return 1;
    case UI_KEY_HOME:
        if (ctrl) uc_move_to(d, 0, shift); else uc_move_home(d, shift);
        return 1;
    case UI_KEY_END:
        if (ctrl) uc_move_to(d, d->len, shift); else uc_move_end(d, shift);
        return 1;
    case UI_KEY_PGUP: uc_move(d, 0, -20, shift, 0); return 1;
    case UI_KEY_PGDN: uc_move(d, 0,  20, shift, 0); return 1;
    case UI_KEY_ENTER: uc_newline(d); uc_api_fire_change(d); return 1;
    case UI_KEY_TAB:   uc_indent(d, shift); return 1;
    case UI_KEY_BACKSPACE: uc_backspace(d); uc_api_fire_change(d); return 1;
    case UI_KEY_DELETE:    uc_del_forward(d); uc_api_fire_change(d); return 1;
    case UI_KEY_ESC:
        if (d->ncur > 1) { uc_clear_extra_cursors(d); return 1; }
        return 0;
    }
    return 0;
}

/* ===========================================================================
 * The find widget.
 * ======================================================================== */
static void find_compile(void)
{
    char err[64];
    if (find_rx) { uc_rx_free(find_rx); find_rx = 0; }
    find_rx_bad = 0;
    if (!find_regex || !find_len) return;
    find_rx = uc_rx_compile(find_str, !find_case, err, sizeof err);
    if (!find_rx) find_rx_bad = 1;
}

/* `upto` bounds the scan.  The painter asks for the matches on ONE line, and
 * an unbounded search per line makes the visible screen cost a pass over the
 * whole document per line - thirty passes over 256 KB, every frame, the moment
 * the find box is open.  Everything else passes d->len and behaves as before. */
static int find_upto(UcDoc *d, int from, int *end, int upto)
{
    int i;
    if (!find_len) return -1;
    if (upto > d->len) upto = d->len;
    if (find_regex) {
        int caps[UC_RX_CAPS * 2];
        if (!find_rx) return -1;
        if (!uc_rx_exec(find_rx, d->text, upto, from, from == 0, caps)) return -1;
        if (end) *end = caps[1] > caps[0] ? caps[1] : caps[0] + 1;
        return caps[0];
    }
    for (i = from; i + find_len <= upto; i++) {
        int k;
        for (k = 0; k < find_len; k++) {
            int a = (unsigned char)d->text[i + k], b = (unsigned char)find_str[k];
            if (!find_case) {
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
            }
            if (a != b) break;
        }
        if (k < find_len) continue;
        if (find_word) {
            if (i > 0 && uc_is_word((unsigned char)d->text[i-1])) continue;
            if (i + find_len < d->len && uc_is_word((unsigned char)d->text[i+find_len])) continue;
        }
        if (end) *end = i + find_len;
        return i;
    }
    return -1;
}

static int find_at(UcDoc *d, int from, int *end)
{
    return find_upto(d, from, end, d->len);
}

static void find_recount(void)
{
    UcDoc *d = uc_doc_active();
    int at = 0, e, n = 0, caret;
    find_total = 0;
    find_index = 0;
    if (!d || !find_len) return;
    caret = d->cur[0].caret;
    while (n < 9999) {
        int hit = find_at(d, at, &e);
        if (hit < 0) break;
        n++;
        if (hit <= caret) find_index = n;
        at = e > hit ? e : hit + 1;
    }
    find_total = n;
}

int uc_find_line_hits(UcDoc *d, int line, int *starts, int *ends, int max)
{
    int s, e, n = 0, at;
    if (!find_on || !find_len || !d) return 0;
    s = uc_line_start(d, line);
    e = uc_line_end(d, line);
    at = s;
    while (n < max) {
        int end2, hit = find_upto(d, at, &end2, e);
        if (hit < 0 || hit >= e) break;
        starts[n] = hit - s;
        ends[n] = (end2 > e ? e : end2) - s;
        n++;
        at = end2 > hit ? end2 : hit + 1;
    }
    return n;
}

void uc_find_set(const char *needle)
{
    uc_scpy(find_str, needle, sizeof find_str);
    find_len = (int)strlen(find_str);
    find_compile();
    find_recount();
}

void uc_find_open(int replace)
{
    UcDoc *d = uc_doc_active();
    /* the widget belongs to the editor, so it takes the editor's focus - it is
     * the only place the characters typed into it can be routed from */
    uc_focus(UC_F_EDITOR);
    find_on = 1;
    find_replace_mode = replace;
    find_focus = 0;
    if (d && uc_has_selection(d)) {
        char sel[120];
        int n = uc_selection_text(d, sel, sizeof sel);
        if (n > 0 && !strchr(sel, '\n')) uc_find_set(sel);
    }
    find_recount();
    uc_ctx_set("findWidgetVisible", 1);
}

void uc_find_close(void)
{
    find_on = 0;
    uc_ctx_set("findWidgetVisible", 0);
}

int uc_find_active(void) { return find_on; }

void uc_find_next(int back)
{
    UcDoc *d = uc_doc_active();
    int from, hit, e;
    if (!d || !find_len) return;
    if (!back) {
        from = d->cur[0].caret;
        hit = find_at(d, from, &e);
        if (hit < 0) hit = find_at(d, 0, &e);         /* wrap */
    } else {
        int at = 0, last = -1, laste = 0, e2;
        while (at < d->cur[0].caret) {
            int h = find_at(d, at, &e2);
            if (h < 0 || h >= d->cur[0].caret) break;
            last = h; laste = e2;
            at = e2 > h ? e2 : h + 1;
        }
        if (last < 0) {                               /* wrap to the last one */
            at = 0;
            while (1) {
                int h = find_at(d, at, &e2);
                if (h < 0) break;
                last = h; laste = e2;
                at = e2 > h ? e2 : h + 1;
            }
        }
        hit = last; e = laste;
    }
    if (hit < 0) { uc_status_msg("No results"); return; }
    d->ncur = 1;
    d->cur[0].anchor = hit;
    d->cur[0].caret = e;
    find_recount();
}

void uc_find_replace(int all)
{
    UcDoc *d = uc_doc_active();
    if (!d || !find_len || d->readonly) return;
    if (all) {
        int at = 0, count = 0;
        uc_begin_group(d);
        while (count < 5000) {
            int e, hit = find_at(d, at, &e);
            if (hit < 0) break;
            uc_replace_range(d, hit, e, repl_str, repl_len);
            at = hit + repl_len;
            count++;
        }
        uc_end_group(d);
        {
            char msg[64], num[16];
            uc_itoa(num, count);
            uc_scpy(msg, num, sizeof msg);
            uc_scat(msg, " replaced", sizeof msg);
            uc_status_msg(msg);
        }
    } else {
        int a = d->cur[0].anchor < d->cur[0].caret ? d->cur[0].anchor : d->cur[0].caret;
        int b = d->cur[0].anchor > d->cur[0].caret ? d->cur[0].anchor : d->cur[0].caret;
        if (b > a) uc_replace_range(d, a, b, repl_str, repl_len);
        uc_find_next(0);
    }
    find_recount();
}

int uc_find_key(int key, int mods, int ch)
{
    char *buf = find_focus ? repl_str : find_str;
    int *len = find_focus ? &repl_len : &find_len;
    int cap = find_focus ? (int)sizeof repl_str : (int)sizeof find_str;
    if (!find_on) return 0;
    if (key == UI_KEY_ESC) { uc_find_close(); return 1; }
    if (key == UI_KEY_ENTER) {
        if (find_focus) uc_find_replace((mods & UI_MOD_CTRL) != 0);
        else uc_find_next((mods & UI_MOD_SHIFT) != 0);
        return 1;
    }
    if (key == UI_KEY_TAB) {
        if (find_replace_mode) find_focus = !find_focus;
        return 1;
    }
    if (key == UI_KEY_BACKSPACE) {
        if (*len > 0) buf[--*len] = 0;
        if (!find_focus) { find_compile(); find_recount(); }
        return 1;
    }
    if (ch >= 32 && ch < 127 && *len < cap - 1) {
        buf[(*len)++] = (char)ch;
        buf[*len] = 0;
        if (!find_focus) { find_compile(); find_recount(); }
        return 1;
    }
    return 1;                     /* the widget swallows everything while open */
}

static UcRect find_rect(UcRect ed)
{
    UcRect r;
    int h = find_replace_mode ? (g_uih + 8) * 2 + 8 : g_uih + 16;
    r.w = 380;
    if (r.w > ed.w - 30) r.w = ed.w - 30;
    r.x = ed.x + ed.w - r.w - UC_BAR_W - 6;
    r.y = ed.y + 2;
    r.h = h;
    return r;
}

static void field_draw(UcRect f, const char *text, int focused, const char *hint)
{
    fb_fill_rect(f.x, f.y, f.w, f.h, uc_col(UC_C_INPUT_BG));
    fb_frame_rect(f.x, f.y, f.w, f.h,
                  focused ? uc_col(UC_C_FOCUS_BORDER) : uc_col(UC_C_INPUT_BORDER));
    fb_set_clip(f.x + 3, f.y, f.w - 6, f.h);
    if (text[0]) uc_ui_text(f.x + 5, f.y + (f.h - g_uih) / 2, text, uc_col(UC_C_INPUT_FG));
    else if (hint) uc_ui_text(f.x + 5, f.y + (f.h - g_uih) / 2, hint, uc_col(UC_C_INPUT_PLACEHOLDER));
    fb_reset_clip();
}

void uc_find_draw(UcRect ed)
{
    UcRect r = find_rect(ed), f;
    char count[40], num[16];
    int fw;
    if (!find_on) return;
    fb_fill_rect(r.x, r.y, r.w, r.h, uc_col(UC_C_WIDGET_BG));
    fb_frame_rect(r.x, r.y, r.w, r.h, uc_col(UC_C_WIDGET_BORDER));
    fw = r.w - 150;      /* leaves room for "12 of 34" beside the toggles */
    f = (UcRect){ r.x + 8, r.y + 6, fw, g_uih + 6 };
    field_draw(f, find_str, !find_focus, "Find");
    if (find_rx_bad)
        uc_ui_text(f.x + f.w + 6, f.y + 3, "bad regex", uc_col(UC_C_ERROR_FG));
    else {
        if (find_total) {
            uc_itoa(num, find_index);
            uc_scpy(count, num, sizeof count);
            uc_scat(count, " of ", sizeof count);
            uc_itoa(num, find_total);
            uc_scat(count, num, sizeof count);
        } else uc_scpy(count, find_len ? "No results" : "", sizeof count);
        uc_ui_text(f.x + f.w + 6, f.y + 3, count, uc_col(UC_C_SIDEBAR_TITLE));
    }
    /* the Aa / ab / .* toggles, as three small letters */
    {
        int bx = r.x + r.w - 62, by = r.y + 6, i;
        static const char *const lbl[3] = { "Aa", "ab", ".*" };
        int on[3];
        on[0] = find_case; on[1] = find_word; on[2] = find_regex;
        for (i = 0; i < 3; i++) {
            int w = 18;
            if (on[i]) fb_fill_rect(bx + i * 20, by, w, g_uih + 6, uc_col(UC_C_LIST_SEL_BG));
            uc_ui_text(bx + i * 20 + 2, by + 3, lbl[i], uc_col(UC_C_SIDEBAR_FG));
        }
    }
    if (find_replace_mode) {
        f = (UcRect){ r.x + 8, r.y + 12 + g_uih + 6, fw, g_uih + 6 };
        field_draw(f, repl_str, find_focus, "Replace");
        uc_ui_text(f.x + f.w + 6, f.y + 3, "Enter / Ctrl+Enter all",
                   uc_col(UC_C_SIDEBAR_TITLE));
    }
}

int uc_find_event(UcRect ed, const unoui_event *e)
{
    UcRect r;
    if (!find_on || e->kind != UI_EV_MOUSE_DOWN) return 0;
    r = find_rect(ed);
    if (e->x < r.x || e->x >= r.x + r.w || e->y < r.y || e->y >= r.y + r.h) return 0;
    {
        int bx = r.x + r.w - 62, by = r.y + 6, i;
        for (i = 0; i < 3; i++)
            if (e->x >= bx + i * 20 && e->x < bx + i * 20 + 18 &&
                e->y >= by && e->y < by + g_uih + 6) {
                if (i == 0) find_case = !find_case;
                if (i == 1) find_word = !find_word;
                if (i == 2) find_regex = !find_regex;
                find_compile();
                find_recount();
                return 1;
            }
    }
    find_focus = (find_replace_mode && e->y > r.y + 8 + g_uih);
    return 1;
}

/* ===========================================================================
 * IntelliSense.
 *
 * Three sources, merged and fuzzy-ranked: the language's keyword list, every
 * distinct word already in the document (which is what makes completion useful
 * in a language nothing knows about), and whatever extension providers return.
 * ======================================================================== */
int uc_suggest_active(void) { return sug_on; }

/* Reading the list back.  The suggestion widget is the one piece of UI whose
 * correctness a screenshot cannot show - "did clangd's members arrive, in the
 * server's order, with the right kinds" is a question about content - so the
 * list is readable and the headless driver prints it (UCD-24). */
int uc_suggest_count(void) { return sug_on ? sug_n : 0; }
int uc_suggest_from_server(void) { return sug_from_server; }
const char *uc_suggest_label(int i)
{ return (i >= 0 && i < sug_n) ? sug[i].label : ""; }
const char *uc_suggest_detail(int i)
{ return (i >= 0 && i < sug_n) ? sug[i].detail : ""; }
const char *uc_suggest_insert(int i)
{ return (i >= 0 && i < sug_n) ? sug[i].insert : ""; }
int uc_suggest_kind_at(int i)
{ return (i >= 0 && i < sug_n) ? sug[i].kind : -1; }
/* The generation bump is what stops a reply that is still in flight from
 * reopening a widget the user has just dismissed (UCD-24). */
static int sug_gen;
void uc_suggest_close(void)
{
    sug_on = 0;
    sug_n = 0;
    sug_gen++;
    uc_ctx_set("suggestWidgetVisible", 0);
}

int uc_suggest_add(const char *label, const char *detail, const char *insert,
                   int kind)
{
    int i;
    if (sug_n >= SUG_MAX || !label || !label[0]) return 0;
    for (i = 0; i < sug_n; i++) if (!strcmp(sug[i].label, label)) return 0;
    uc_scpy(sug[sug_n].label, label, sizeof sug[0].label);
    uc_scpy(sug[sug_n].detail, detail ? detail : "", sizeof sug[0].detail);
    uc_scpy(sug[sug_n].insert, insert && insert[0] ? insert : label, sizeof sug[0].insert);
    sug[sug_n].kind = kind;
    sug[sug_n].score = uc_fuzzy(sug_prefix, label, 0, 0);
    sug_n++;
    return 1;
}

static void collect_words(UcDoc *d)
{
    int i = 0;
    while (i < d->len && sug_n < SUG_MAX) {
        int s;
        if (!uc_is_word((unsigned char)d->text[i]) || (d->text[i] >= '0' && d->text[i] <= '9')) { i++; continue; }
        s = i;
        while (i < d->len && uc_is_word((unsigned char)d->text[i])) i++;
        if (i - s >= 3 && i - s < 40) {
            char w[44];
            int n = i - s;
            memcpy(w, d->text + s, (unsigned long)n);
            w[n] = 0;
            if (uc_fuzzy(sug_prefix, w, 0, 0) >= 0) uc_suggest_add(w, "", w, UC_CI_TEXT);
        }
    }
}

static void collect_keywords(UcDoc *d)
{
    UcLang *L = uc_lang_at(d->lang);
    const char *p;
    if (!L || !L->keywords) return;
    for (p = L->keywords; *p; ) {
        char w[40];
        int n = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && n < (int)sizeof w - 1) w[n++] = *p++;
        w[n] = 0;
        if (n && uc_fuzzy(sug_prefix, w, 0, 0) >= 0)
            uc_suggest_add(w, L->name, w, UC_CI_KEYWORD);
    }
}

static void sug_sort(void)
{
    int i, j;
    for (i = 1; i < sug_n; i++) {
        UcSuggest t = sug[i];
        for (j = i - 1; j >= 0 && sug[j].score < t.score; j--) sug[j + 1] = sug[j];
        sug[j + 1] = t;
    }
}

void uc_suggest_open(UcDoc *d, int explicit_req)
{
    int c;
    if (!d) return;
    c = d->cur[d->ncur - 1].caret;
    sug_word_start = uc_word_start(d, c);
    {
        int n = c - sug_word_start;
        if (n > (int)sizeof sug_prefix - 1) n = (int)sizeof sug_prefix - 1;
        memcpy(sug_prefix, d->text + sug_word_start, (unsigned long)n);
        sug_prefix[n] = 0;
    }
    /* A new request context, so any reply still in flight is now stale
     * (UCD-24).  Bumped BEFORE the early return, or closing the widget by
     * typing a space would leave the previous generation current and let its
     * reply reopen the widget. */
    sug_gen++;
    if (!explicit_req && !sug_prefix[0]) { uc_suggest_close(); return; }
    sug_n = 0;
    sug_sel = 0;
    sug_scroll = 0;
    sug_from_server = 0;
    uc_api_completions(d, c);
    uc_ext_snippets(d, sug_prefix);
    collect_keywords(d);
    collect_words(d);
    /* drop the word being typed: offering it back is noise */
    {
        int i;
        for (i = 0; i < sug_n; i++)
            if (!strcmp(sug[i].label, sug_prefix)) {
                int k;
                for (k = i; k < sug_n - 1; k++) sug[k] = sug[k + 1];
                sug_n--;
                break;
            }
    }
    sug_sort();
    /* Ask the server too.  The local list is what is on screen NOW; the
     * server's answer replaces it when it arrives.  An explicit Ctrl+Space with
     * a server attached opens the widget even when the local sources found
     * nothing, because something is about to arrive and a widget that flickered
     * shut and back open would be worse than one that is briefly empty. */
    sug_on = sug_n > 0 || uc_lsp_for_doc(d) != 0;
    sug_ask_server(d, c);
    uc_ctx_set("suggestWidgetVisible", sug_on);
}

void uc_suggest_retrigger(UcDoc *d) { uc_suggest_open(d, 0); }

/* ---------------------------------------------------------------------------
 * Completions from a language server (UCD-24).
 *
 * THE WIDGET IS SYNCHRONOUS AND THE PROTOCOL IS NOT.  uc_suggest_open() has to
 * return with something on screen; the server answers frames later.  So the
 * local sources still run first and the widget opens on them, and the server's
 * answer REPLACES the list when it lands.  A widget that waited would either
 * block the frame loop or flash empty for a hundred milliseconds on every
 * keystroke, and both are worse than a list that improves.
 *
 * A REPLY THAT ARRIVES TOO LATE MUST BE THROWN AWAY.  The user keeps typing
 * while the request is in flight, so by the time it answers the prefix may be
 * different, the widget closed, or the document changed.  Every request carries
 * the generation it was sent in; a reply whose generation is no longer current
 * is dropped.  Comparing "is a request outstanding" instead would accept an old
 * reply that overtook a newer one, and the symptom - a completion list for a
 * prefix you have already finished typing - is one nobody would think to blame
 * on ordering. */
#define SUG_REQ 8
static int sug_req[SUG_REQ];           /* the generation each slot was sent in */
static int sug_req_at;

/* LSP's CompletionItemKind is 1..25 and UnoCode's UC_CI_* is 0..8, and they
 * are NOT the same numbering even where the names line up - LSP's Text is 1
 * and ours is 0, so a straight cast is wrong for every single value.  Indexed
 * by the LSP kind; entry 0 is unused because LSP starts at 1. */
static const unsigned char kKindMap[26] = {
    UC_CI_TEXT,                                     /*  0 unused           */
    UC_CI_TEXT,      UC_CI_METHOD,   UC_CI_FUNCTION, /*  1 Text  2 Method  3 Function */
    UC_CI_FUNCTION,  UC_CI_PROPERTY, UC_CI_VARIABLE, /*  4 Ctor  5 Field   6 Variable */
    UC_CI_CLASS,     UC_CI_CLASS,    UC_CI_CLASS,    /*  7 Class 8 Iface   9 Module   */
    UC_CI_PROPERTY,  UC_CI_TEXT,     UC_CI_VARIABLE, /* 10 Prop 11 Unit   12 Value    */
    UC_CI_CLASS,     UC_CI_KEYWORD,  UC_CI_SNIPPET,  /* 13 Enum 14 Keyw   15 Snippet  */
    UC_CI_TEXT,      UC_CI_FILE,     UC_CI_TEXT,     /* 16 Colour 17 File 18 Ref      */
    UC_CI_FILE,      UC_CI_PROPERTY, UC_CI_VARIABLE, /* 19 Folder 20 EnumMem 21 Const */
    UC_CI_CLASS,     UC_CI_METHOD,   UC_CI_KEYWORD,  /* 22 Struct 23 Event 24 Operator*/
    UC_CI_CLASS                                      /* 25 TypeParameter              */
};

static int lsp_kind(int k)
{
    if (k < 1 || k > 25) return UC_CI_TEXT;
    return kKindMap[k];
}

/* What to actually insert.  textEdit.newText wins over insertText wins over the
 * label; the edit's RANGE is deliberately ignored and sug_word_start is used
 * instead.  For a word completion the two are the same range, and honouring an
 * edit range computed against the text as it was when the request went out
 * would apply it to text that has since been typed into. */
static const char *item_insert(UcJson *it)
{
    UcJson *te = uc_json_member(it, "textEdit");
    const char *s;
    if (te) {
        s = uc_json_str(te, "newText", 0);
        if (s && s[0]) return s;
    }
    s = uc_json_str(it, "insertText", 0);
    if (s && s[0]) return s;
    return uc_json_str(it, "label", "");
}

/* The server's own detail line.  clangd puts the return type in `detail` and
 * the parameter list in `labelDetails.detail`; pyright uses `detail` alone. */
static void item_detail(UcJson *it, char *out, int cap)
{
    UcJson *ld = uc_json_member(it, "labelDetails");
    const char *d = uc_json_str(it, "detail", "");
    uc_scpy(out, d ? d : "", cap);
    if (ld) {
        const char *sfx = uc_json_str(ld, "detail", "");
        if (sfx && sfx[0]) {
            if (out[0]) uc_scat(out, " ", cap);
            uc_scat(out, sfx, cap);
        }
    }
}

static void sug_lsp_reply(UcJson *result, UcJson *error, void *user)
{
    int gen = *(int *)user;
    UcJson *items, *it;
    int rank;
    if (gen != sug_gen || !sug_on) return;   /* the moment has passed */

    /* NOTHING TO OFFER STILL HAS TO BE ANSWERED.  The widget may have been
     * opened empty on the strength of a server being attached, so a reply with
     * no items - or an error, or the (0,0) that means the server died with the
     * request outstanding - has to close it.  Left alone it is an empty box
     * that never goes away and swallows Tab and Escape. */
    items = 0;
    if (result && !error)
        items = (result->type == UJ_ARR) ? result
                                         : uc_json_member(result, "items");
    if (!items || items->type != UJ_ARR || items->n <= 0) {
        if (!sug_n) { uc_suggest_close(); uc_repaint(); }
        return;
    }

    sug_n = 0;
    sug_sel = 0;
    sug_scroll = 0;
    sug_from_server = 1;
    rank = 0;
    for (it = items->child; it && sug_n < SUG_MAX; it = it->next) {
        const char *label = uc_json_str(it, "label", "");
        char detail[96];
        int i = sug_n;
        if (!label || !label[0]) continue;
        /* clangd pads a label with a leading space to sort it; it is not part
         * of the name and inserting it would be wrong */
        while (*label == ' ') label++;
        item_detail(it, detail, (int)sizeof detail);
        if (!uc_suggest_add(label, detail, item_insert(it),
                            lsp_kind((int)uc_json_num(it, "kind", 1))))
            continue;
        /* The SERVER'S order, not the fuzzy matcher's.  It knows which
         * overload is in scope and which member is private; a rank derived
         * from the prefix knows only how the letters line up, and would put an
         * unreachable symbol above the obvious one whenever the spelling
         * happened to suit it better. */
        sug[i].score = 100000 - rank++;
    }
    sug_sort();
    sug_on = sug_n > 0;
    uc_ctx_set("suggestWidgetVisible", sug_on);
    uc_repaint();
}

/* Is the character just before the caret one the SERVER declared as a trigger?
 *
 * Asked of the server's capabilities rather than assumed, because the set is
 * per-language and not guessable: clangd registers ".", ">", ":" and "\"",
 * pyright registers "." and "[", and a hard-coded "." would silently mis-report
 * every other one. */
static int trigger_char_at(UcDoc *d, int caret, char *out)
{
    UcLsp *s = uc_lsp_for_doc(d);
    UcJson *caps = s ? uc_lsp_caps(s) : 0;
    UcJson *cp = caps ? uc_json_member(caps, "completionProvider") : 0;
    UcJson *tc = cp ? uc_json_member(cp, "triggerCharacters") : 0;
    UcJson *it;
    char prev;
    *out = 0;
    if (!tc || tc->type != UJ_ARR || caret <= 0) return 0;
    prev = d->text[caret - 1];
    for (it = tc->child; it; it = it->next)
        if (it->type == UJ_STR && it->str && it->str[0] == prev && !it->str[1]) {
            *out = prev;
            return 1;
        }
    return 0;
}

/* ===========================================================================
 * Hover (UCD-25).
 * ======================================================================== */
/* The popup.  Painted over the workbench rather than inside the editor's clip,
 * because a hover near the right edge of a split editor otherwise gets cut in
 * half by a rect it has no reason to respect. */
void uc_hover_draw(UcRect wb)
{
    const char *p = hov_text;
    int rows = 0, w = 0, i, x, y, h, lh = uc_ui_h() + 2;
    char line[220];
    if (!hov_on || !hov_text[0]) return;

    /* measure: the widest line and the number of them, both capped */
    while (*p && rows < UC_HOVER_ROWS) {
        int n = 0;
        while (*p && *p != '\n' && n < (int)sizeof line - 1) line[n++] = *p++;
        line[n] = 0;
        if (*p == '\n') p++;
        if (n) { int tw = uc_ui_text_w(line); if (tw > w) w = tw; }
        rows++;
    }
    if (!rows) return;
    w += 20;
    if (w < 160) w = 160;
    if (w > wb.w - 40) w = wb.w - 40;
    h = rows * lh + 12;

    /* Above the pointer by preference - a box below it covers the line you are
     * about to read next, and the pointer is already at the bottom edge of the
     * thing being asked about. */
    x = hov_x - 8;
    y = hov_y - h - 6;
    if (y < wb.y + 4) y = hov_y + uc_line_h() + 4;
    if (x + w > wb.x + wb.w - 8) x = wb.x + wb.w - w - 8;
    if (x < wb.x + 4) x = wb.x + 4;
    if (y + h > wb.y + wb.h - 4) y = wb.y + wb.h - h - 4;

    fb_blend_rect(x + 3, y + 3, w, h, uc_col(UC_C_WIDGET_SHADOW), 70);
    fb_fill_rect(x, y, w, h, uc_col(UC_C_WIDGET_BG));
    fb_frame_rect(x, y, w, h, uc_col(UC_C_WIDGET_BORDER));
    fb_set_clip(x + 1, y + 1, w - 2, h - 2);
    p = hov_text;
    for (i = 0; i < rows; i++) {
        int n = 0;
        while (*p && *p != '\n' && n < (int)sizeof line - 1) line[n++] = *p++;
        line[n] = 0;
        if (*p == '\n') p++;
        uc_ui_text_fit(x + 10, y + 6 + i * lh, line, w - 20,
                       uc_col(UC_C_EDITOR_FG));
    }
    fb_reset_clip();
}

void uc_hover_close(void)
{
    if (!hov_on && !hov_pending) return;
    hov_on = 0;
    hov_pending = 0;
    hov_gen++;                     /* drop any reply still on its way */
    hov_text[0] = 0;
    uc_ctx_set("hoverVisible", 0);
    uc_repaint();
}

int uc_hover_active(void) { return hov_on; }
const char *uc_hover_text(void) { return hov_text; }

/* Append `s` to the popup text, undoing the markdown a server wraps its answer
 * in.  Fences and their language tag go entirely; a leading `#` or a run of `*`
 * is emphasis, not content.  Nothing else is interpreted - a stray underscore
 * in an identifier is an underscore. */
static void hov_append_md(const char *s)
{
    int n = (int)strlen(hov_text), bol = 1;
    if (!s) return;
    if (n && n < UC_HOVER_MAX - 2) hov_text[n++] = '\n';
    while (*s && n < UC_HOVER_MAX - 2) {
        if (bol && s[0] == '`' && s[1] == '`' && s[2] == '`') {
            s += 3;
            while (*s && *s != '\n') s++;   /* the language tag */
            if (*s) s++;
            continue;
        }
        if (bol) {
            while (*s == '#' || (*s == ' ' && s[1] == '#')) s++;
            while (*s == ' ') s++;
            bol = 0;
            if (!*s) break;
            continue;
        }
        if (*s == '*' || *s == '`') { s++; continue; }
        if (*s == '\n') bol = 1;
        hov_text[n++] = *s++;
    }
    hov_text[n] = 0;
}

/* `contents` is a MarkupContent, a MarkedString, a plain string, or an array of
 * any of those.  All four are legal and servers differ, so all four are read. */
static void hov_read_contents(UcJson *c)
{
    UcJson *it;
    if (!c) return;
    if (c->type == UJ_STR) { hov_append_md(c->str); return; }
    if (c->type == UJ_ARR) {
        for (it = c->child; it; it = it->next) hov_read_contents(it);
        return;
    }
    if (c->type == UJ_OBJ) {
        const char *v = uc_json_str(c, "value", 0);
        if (v) hov_append_md(v);
    }
}

static void hov_reply(UcJson *result, UcJson *error, void *user)
{
    int gen = *(int *)user;
    if (gen != hov_gen) return;
    hov_pending = 0;
    hov_text[0] = 0;
    if (!result || error || result->type == UJ_NULL) return;
    hov_read_contents(uc_json_member(result, "contents"));
    /* Nothing to say is not the same as a box saying nothing.  clangd answers
     * with an empty hover for whitespace and punctuation, which is most of
     * where a pointer rests. */
    if (!hov_text[0]) return;
    hov_on = 1;
    uc_ctx_set("hoverVisible", 1);
    uc_repaint();
}

static int hov_req[4];
static int hov_req_at;

/* Ask about `off`, anchoring the popup at (px, py). */
void uc_hover_at(UcDoc *d, int off, int px, int py)
{
    int *slot;
    if (!d || !uc_lsp_for_doc(d)) return;
    hov_gen++;
    hov_on = 0;
    hov_text[0] = 0;
    hov_off = off;
    hov_x = px;
    hov_y = py;
    slot = &hov_req[hov_req_at];
    hov_req_at = (hov_req_at + 1) % 4;
    *slot = hov_gen;
    hov_pending = 1;
    if (!uc_lsp_request_at(d, "textDocument/hover", off, 0, hov_reply, slot))
        hov_pending = 0;
}

/* Note where the pointer is.  Called from the editor's mouse-move handler; the
 * request itself waits for the pointer to stop. */
static void hov_pointer(UcRect r, UcDoc *d, int px, int py)
{
    int off = offset_at(r, d, px, py);
    if (hov_on && off == hov_off) return;     /* still on the same word */
    if (hov_on || hov_pending) uc_hover_close();
    hov_want_x = px;
    hov_want_y = py;
    hov_dwell_at = uno_dbg_uptime_ms() + UC_HOVER_MS;
}

/* Once a frame: fire the request when the pointer has been still long enough. */
void uc_hover_tick(UcDoc *d)
{
    UcRect r;
    int off;
    if (!d || !hov_dwell_at || hov_on || hov_pending) return;
    if (uno_dbg_uptime_ms() < hov_dwell_at) return;
    hov_dwell_at = 0;
    if (!uc_cfg_bool("editor.hover.enabled")) return;
    r = UC.editor;
    off = offset_at(r, d, hov_want_x, hov_want_y);
    /* Only over a word.  Hovering the gutter, a blank line or a comma asks a
     * question with no answer, and a request per resting pointer is a request
     * per idle second. */
    if (off < 0 || off >= d->len) return;
    if (!uc_is_word((unsigned char)d->text[off])) return;
    uc_hover_at(d, off, hov_want_x, hov_want_y);
}

/* Fire a completion request for the caret, if a server is serving this file. */
static void sug_ask_server(UcDoc *d, int caret)
{
    char extra[64], trig = 0;
    int *slot;
    if (!uc_lsp_for_doc(d)) return;
    slot = &sug_req[sug_req_at];
    sug_req_at = (sug_req_at + 1) % SUG_REQ;
    *slot = sug_gen;
    /* triggerKind 1 = Invoked, 2 = TriggerCharacter.  Servers use it to decide
     * whether to offer everything in scope or only what may follow the
     * character - the difference between a member list and a translation unit -
     * so it is worth reporting truthfully rather than conveniently. */
    if (trigger_char_at(d, caret, &trig)) {
        char esc[8];
        esc[0] = trig; esc[1] = 0;
        uc_scpy(extra, ",\"context\":{\"triggerKind\":2,\"triggerCharacter\":\"",
                (int)sizeof extra);
        if (trig == '"' || trig == '\\') uc_scat(extra, "\\", (int)sizeof extra);
        uc_scat(extra, esc, (int)sizeof extra);
        uc_scat(extra, "\"}", (int)sizeof extra);
    } else {
        uc_scpy(extra, ",\"context\":{\"triggerKind\":1}", (int)sizeof extra);
    }
    uc_lsp_request_at(d, "textDocument/completion", caret, extra,
                      sug_lsp_reply, slot);
}

static void sug_accept(UcDoc *d)
{
    const char *ins;
    if (!sug_on || sug_sel < 0 || sug_sel >= sug_n) return;
    ins = sug[sug_sel].insert;
    uc_replace_range(d, sug_word_start, d->cur[d->ncur - 1].caret, ins, (int)strlen(ins));
    uc_suggest_close();
}

int uc_suggest_key(int key, int mods)
{
    UcDoc *d = uc_doc_active();
    if (!sug_on) return 0;
    switch (key) {
    case UI_KEY_UP:   if (sug_sel > 0) sug_sel--; return 1;
    case UI_KEY_DOWN: if (sug_sel < sug_n - 1) sug_sel++; return 1;
    case UI_KEY_ESC:  uc_suggest_close(); return 1;
    case UI_KEY_TAB:  sug_accept(d); return 1;
    case UI_KEY_ENTER:
        if (uc_cfg_bool("editor.acceptSuggestionOnEnter")) { sug_accept(d); return 1; }
        uc_suggest_close();
        return 0;
    default: break;
    }
    (void)mods;
    return 0;
}

static const char *kind_mark(int k)
{
    switch (k) {
    case UC_CI_METHOD:   return "m";
    case UC_CI_FUNCTION: return "f";
    case UC_CI_VARIABLE: return "v";
    case UC_CI_CLASS:    return "C";
    case UC_CI_KEYWORD:  return "k";
    case UC_CI_SNIPPET:  return "S";
    case UC_CI_FILE:     return "F";
    case UC_CI_PROPERTY: return "p";
    default:             return "\xc2\xb7";
    }
}

void uc_suggest_draw(UcRect ed, UcDoc *d)
{
    EdLayout L;
    int line, vcol, x, y, i, rows, h, w = 300;
    /* sug_on can be set with an empty list: the widget opens on the strength of
     * a server being attached and fills when the reply lands (UCD-24).  Drawing
     * an empty box in the meantime would flash a grey rectangle under the caret
     * on every keystroke. */
    if (!sug_on || sug_n <= 0 || !d) return;
    /* A server's labels and signatures are far wider than a scraped word, and
     * a fixed 300 shows `operator<<` next to a truncated `std::basic_ostream`.
     * Measure the widest row and grow, still bounded by the editor. */
    for (i = 0; i < sug_n; i++) {
        int need = 40 + uc_ui_text_w(sug[i].label);
        if (sug[i].detail[0]) need += uc_ui_text_w(sug[i].detail) + 20;
        if (need > w) w = need;
    }
    if (w > 720) w = 720;
    L = ed_layout(ed, d);
    caret_col_of(d, d->cur[d->ncur - 1].caret, &line, &vcol);
    x = L.text.x + (vcol - d->scroll_col) * g_cw - 20;
    y = ed.y + (line - d->scroll_line + 1) * g_lh;
    /* The widget must fit INSIDE the editor rect: below the caret if there is
     * room, above it if there is more room there, and with the row count cut
     * to whatever is left either way.  Clamping only the position leaves it
     * drawing over the panel below, which is what a short editor pane on a
     * 640x400 desktop does every time. */
    {
        int rh = g_uih + 4;
        int below = (ed.y + ed.h) - y;
        int above = (ed.y + (line - d->scroll_line) * g_lh) - ed.y;
        int room  = below >= above ? below : above;
        int fit   = (room - 6) / rh;
        rows = sug_n > 9 ? 9 : sug_n;
        if (fit < 1) fit = 1;
        if (rows > fit) rows = fit;
        h = rows * rh + 6;
        if (below < h && above >= h) y = ed.y + (line - d->scroll_line) * g_lh - h;
    }
    if (w > ed.w - 20) w = ed.w - 20;
    if (x + w > ed.x + ed.w) x = ed.x + ed.w - w;
    if (x < ed.x) x = ed.x;
    if (y + h > ed.y + ed.h) y = ed.y + ed.h - h;
    if (y < ed.y) y = ed.y;
    if (sug_sel < sug_scroll) sug_scroll = sug_sel;
    if (sug_sel >= sug_scroll + rows) sug_scroll = sug_sel - rows + 1;

    fb_fill_rect(x, y, w, h, uc_col(UC_C_SUGGEST_BG));
    fb_frame_rect(x, y, w, h, uc_col(UC_C_SUGGEST_BORDER));
    for (i = 0; i < rows; i++) {
        int k = sug_scroll + i;
        int ry = y + 3 + i * (g_uih + 4);
        if (k >= sug_n) break;
        if (k == sug_sel) fb_fill_rect(x + 1, ry - 1, w - 2, g_uih + 4, uc_col(UC_C_SUGGEST_SEL_BG));
        uc_ui_text(x + 6, ry + 1, kind_mark(sug[k].kind), uc_col(UC_C_LIST_HIGHLIGHT));
        /* the detail is right-aligned and the label runs towards it, so the
         * LABEL is the one that gets fitted - a completion whose name is cut
         * short is still readable, one written over its own description is not */
        {
            int dw = sug[k].detail[0] ? uc_ui_text_w(sug[k].detail) + 14 : 8;
            uc_ui_text_fit(x + 22, ry + 1, sug[k].label, w - 30 - dw,
                           k == sug_sel ? uc_col(UC_C_LIST_SEL_FG)
                                        : uc_col(UC_C_EDITOR_FG));
            if (sug[k].detail[0])
                uc_ui_text(x + w - dw + 6, ry + 1, sug[k].detail,
                           uc_col(UC_C_BREADCRUMB_FG));
        }
    }
}

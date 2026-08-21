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
    char label[48];
    char detail[40];
    char insert[80];
    int  kind;
    int  score;
} UcSuggest;
static UcSuggest sug[SUG_MAX];
static int  sug_n, sug_sel, sug_on, sug_scroll;
static int  sug_word_start;
static char sug_prefix[48];

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
}

static void draw_vscroll(UcRect b, UcDoc *d, int first_row, int rows)
{
    int nl = uc_line_count(d), th, ty;
    if (b.w <= 0) return;
    fb_fill_rect(b.x, b.y, b.w, b.h, uc_col(UC_C_EDITOR_BG));
    if (nl <= rows) return;
    th = b.h * rows / nl;
    if (th < 20) th = 20;
    ty = b.y + (b.h - th) * first_row / (nl - rows);
    fb_fill_rect(b.x + 2, ty, b.w - 4, th, uc_col(UC_C_SCROLL_SLIDER));
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

    if (find_on) uc_find_draw(r);
    if (sug_on)  uc_suggest_draw(r, d);
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
void uc_suggest_close(void) { sug_on = 0; sug_n = 0; uc_ctx_set("suggestWidgetVisible", 0); }

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
    if (!explicit_req && !sug_prefix[0]) { uc_suggest_close(); return; }
    sug_n = 0;
    sug_sel = 0;
    sug_scroll = 0;
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
    sug_on = sug_n > 0;
    uc_ctx_set("suggestWidgetVisible", sug_on);
}

void uc_suggest_retrigger(UcDoc *d) { uc_suggest_open(d, 0); }

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
    if (!sug_on || !d) return;
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

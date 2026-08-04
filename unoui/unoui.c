/* ===========================================================================
 * unoui core - window building, the shared depth-aware drawing helpers, the
 * portable default painters, and the render dispatcher (with per-painter NULL
 * fallback to the defaults). Pure C over fb.h: builds for every UnoDOS C port
 * and the host harness unchanged.
 * ===========================================================================
 */
#include "unoui_theme.h"
#include <string.h>

/* optional per-app icon artwork hook (NULL = use the theme's generic glyph) */
unoui_icon_fn unoui_icon_art = 0;

/* optional desktop wallpaper hook (NULL = each theme paints its own desktop) */
unoui_wallpaper_fn unoui_wallpaper = 0;

/* ---- font-derived metrics (see unoui_theme.h) ----------------------------- *
 * Every pitch keeps its historic value under the 8px bitmap font and scales
 * with a TTF. Floors preserve minimum touch-target sizes. */
int ui_line_h(void)    { return fb_text_h() + 2; }
int ui_row_h(void)     { return fb_text_h() + 3; }
int ui_prow_h(void)    { return fb_text_h() + 4; }
int ui_menubar_h(void) { int h = fb_text_h() + 7;  return h < 15 ? 15 : h; }
int ui_tab_h(void)     { int h = fb_text_h() + 10; return h < 18 ? 18 : h; }
int ui_ctl_h(void)     { int h = fb_text_h() + 8;  return h < 16 ? 16 : h; }
int ui_field_h(void)   { int h = fb_text_h() + 6;  return h < 16 ? 16 : h; }

/* pixel width of the byte range [s,e) of `buf` in the active font. Measured in
 * bounded chunks so unterminated ranges of any length work; the edit painter
 * DRAWS with the same chunking, keeping measure and paint in exact lockstep. */
#define UI_SEG_CHUNK 96
int ui_seg_w(const char *buf, int s, int e)
{
    char tmp[UI_SEG_CHUNK + 1];
    int w = 0;
    while (s < e) {
        int n = e - s > UI_SEG_CHUNK ? UI_SEG_CHUNK : e - s;
        memcpy(tmp, buf + s, (size_t)n); tmp[n] = 0;
        w += fb_text_w(tmp);
        s += n;
    }
    return w;
}

/* draw the byte range [s,e) at (x,y); returns the advanced x (same chunking) */
static int seg_draw(const char *buf, int s, int e, int x, int y, fb_px fg, long bg)
{
    char tmp[UI_SEG_CHUNK + 1];
    while (s < e) {
        int n = e - s > UI_SEG_CHUNK ? UI_SEG_CHUNK : e - s;
        memcpy(tmp, buf + s, (size_t)n); tmp[n] = 0;
        x = fb_text(x, y, tmp, fg, bg);
        s += n;
    }
    return x;
}

/* ---- masked text ----------------------------------------------------------
 * A secret field draws N mask characters where the real text is. Everything
 * that measures or paints an editable range goes through these two, so the
 * caret, the click hit test, the horizontal scroll and the glyphs are all
 * talking about the SAME string - which is what makes a click land on the
 * right character in a password field as reliably as in a plain one.
 *
 * The mask run is measured, never multiplied: a proportional face kerns its
 * own mask character against itself, so N * width(mask) is not width of N
 * masks. Chunked exactly like ui_seg_w for the same reason it is. */
static int masked(const unoui_text *t) { return t && t->secret && !t->revealed; }

static int mask_w(int mask_char, int n)
{
    char tmp[UI_SEG_CHUNK + 1];
    int w = 0;
    while (n > 0) {
        int k = n > UI_SEG_CHUNK ? UI_SEG_CHUNK : n, i;
        for (i = 0; i < k; i++) tmp[i] = (char)mask_char;
        tmp[k] = 0;
        w += fb_text_w(tmp);
        n -= k;
    }
    return w;
}

static int mask_draw(int mask_char, int n, int x, int y, fb_px fg, long bg)
{
    char tmp[UI_SEG_CHUNK + 1];
    while (n > 0) {
        int k = n > UI_SEG_CHUNK ? UI_SEG_CHUNK : n, i;
        for (i = 0; i < k; i++) tmp[i] = (char)mask_char;
        tmp[k] = 0;
        x = fb_text(x, y, tmp, fg, bg);
        n -= k;
    }
    return x;
}

/* width of [s,e) as the field actually shows it */
static int t_seg_w(const unoui_text *t, int s, int e)
{ return masked(t) ? mask_w(t->secret, e - s) : ui_seg_w(t->buf, s, e); }

static int t_seg_draw(const unoui_text *t, int s, int e, int x, int y,
                      fb_px fg, long bg)
{ return masked(t) ? mask_draw(t->secret, e - s, x, y, fg, bg)
                   : seg_draw(t->buf, s, e, x, y, fg, bg); }

/* ------------------------------------------------------------------ build -- */

/* S-UUI-04: on overflow, hand back a THROWAWAY scratch widget, not the live
 * slot 63. The old code returned &win->w[MAX-1] without bumping nw or zeroing
 * it, so an over-widgeted window silently corrupted whatever real widget sat
 * in the last slot (and every caller after kept overwriting it). A scratch
 * cell is discarded when the window renders, so the excess widget simply
 * doesn't appear - visibly wrong, but never memory corruption. */
static unoui_widget g_widget_scratch;
static unoui_widget *push(unoui_window *win, ui_kind k, int x, int y,
                          int w, int h, const char *text)
{
    unoui_widget *wd;
    if (win->nw >= UNOUI_MAX_WIDGETS) {
        memset(&g_widget_scratch, 0, sizeof g_widget_scratch);
        g_widget_scratch.kind = k;
        return &g_widget_scratch;
    }
    wd = &win->w[win->nw++];
    memset(wd, 0, sizeof(*wd));
    wd->kind = k; wd->r.x = x; wd->r.y = y; wd->r.w = w; wd->r.h = h;
    wd->text = text;
    return wd;
}

void unoui_window_init(unoui_window *win, const char *title,
                       int x, int y, int w, int h)
{
    memset(win, 0, sizeof(*win));
    win->title = title;
    win->r.x = x; win->r.y = y; win->r.w = w; win->r.h = h;
    win->active = 1;
    win->font_slot = UI_FONT_INHERIT;
    win->min_w = 140; win->min_h = 100;
}

/* per-window font override hooks (platform-supplied; default no-ops) */
void (*unoui_font_push)(int slot) = 0;
void (*unoui_font_pop)(void) = 0;

void unoui_widget_fill(unoui_widget *w) { if (w) w->flags |= UI_WF_FILL; }

/* stretch every fill widget to the window's current content rect (called on
 * resize + resolution change). content = the region below the titlebar/frame. */
void unoui_reflow_window(const unoui_theme *t, unoui_window *win)
{
    int bare = (win->flags & UI_WIN_BARE) != 0;
    int fw = t->m.frame_w, th = bare ? 0 : t->m.title_h, pad = bare ? 0 : t->m.pad;
    int cw = win->r.w - 2 * fw - 2 * pad;         /* usable content (matches origin) */
    int ch = win->r.h - th - fw - 2 * pad, i;
    /* FILL means "take the usable content", and the scrollbar's strip is not
     * usable - a filling widget that reached under it would be drawn beneath a
     * bar and hit-tested through it. A scrolling window's fill height is the
     * CONTENT height too, not the frame's: filling to the frame would make the
     * thing that scrolls exactly as tall as the hole it scrolls in. */
    if (win->flags & UI_WIN_VSCROLL) {
        cw -= UI_WIN_BAR_W;
        if (win->content_h > ch) ch = win->content_h;
    }
    for (i = 0; i < win->nw; i++) {
        unoui_widget *w = &win->w[i];
        if (!(w->flags & UI_WF_FILL)) continue;
        w->r.w = cw - w->r.x;  if (w->r.w < 1) w->r.w = 1;
        w->r.h = ch - w->r.y;  if (w->r.h < 1) w->r.h = 1;
    }
}

unoui_widget *unoui_add_label(unoui_window *w, int x, int y, const char *t)
{ return push(w, UI_LABEL, x, y, fb_text_w(t), fb_text_h(), t); }

unoui_widget *unoui_add_button(unoui_window *w, int x, int y, int ww,
                               const char *t, int flags)
{ unoui_widget *d = push(w, UI_BUTTON, x, y, ww, ui_ctl_h(), t); d->flags = flags; return d; }

unoui_widget *unoui_add_check(unoui_window *w, int x, int y, const char *t, int on)
{ int h = fb_text_h() > 12 ? fb_text_h() : 12;
  unoui_widget *d = push(w, UI_CHECK, x, y, 12 + fb_text_w(t) + 6, h, t);
  d->flags = on ? UI_F_CHECKED : 0;
  return d; }

unoui_widget *unoui_add_radio(unoui_window *w, int x, int y, const char *t, int on)
{ int h = fb_text_h() > 12 ? fb_text_h() : 12;
  unoui_widget *d = push(w, UI_RADIO, x, y, 12 + fb_text_w(t) + 6, h, t);
  d->flags = on ? UI_F_CHECKED : 0;
  return d; }

unoui_widget *unoui_add_field(unoui_window *w, int x, int y, int ww,
                              const char *t, int focus)
{ unoui_widget *d = push(w, UI_FIELD, x, y, ww, ui_field_h(), t);
  d->flags = focus ? UI_F_FOCUS : 0;
  return d; }

unoui_widget *unoui_add_progress(unoui_window *w, int x, int y, int ww,
                                 int v, int vm)
{ unoui_widget *d = push(w, UI_PROGRESS, x, y, ww, 12, 0);
  d->value = v; d->vmax = vm; return d; }

unoui_widget *unoui_add_busy(unoui_window *w, int x, int y, int size)
{ int s = size > 0 ? size : fb_text_h() + 6;
  unoui_widget *d = push(w, UI_BUSY, x, y, s, s, 0);
  d->vmax = UI_BUSY_DOTS; return d; }

void unoui_busy_step(unoui_widget *w)
{ if (w && w->kind == UI_BUSY) w->value = (w->value + 1) % UI_BUSY_DOTS; }

unoui_widget *unoui_add_vscroll(unoui_window *w, int x, int y, int h, int v, int vm)
{ unoui_widget *d = push(w, UI_VSCROLL, x, y, 14, h, 0);
  d->value = v; d->vmax = vm; return d; }

unoui_widget *unoui_add_list(unoui_window *w, int x, int y, int ww, int h,
                             const char **items, int n, int sel)
{ unoui_widget *d = push(w, UI_LIST, x, y, ww, h, 0);
  d->items = items; d->nitems = n; d->sel = sel; return d; }

unoui_widget *unoui_add_group(unoui_window *w, int x, int y, int ww, int h,
                              const char *t)
{ return push(w, UI_GROUP, x, y, ww, h, t); }

unoui_widget *unoui_add_sep(unoui_window *w, int x, int y, int ww)
{ return push(w, UI_SEP, x, y, ww, 2, 0); }

unoui_widget *unoui_add_icon(unoui_window *w, int x, int y, const char *t)
{ return push(w, UI_ICON, x, y, 48, 44, t); }

unoui_widget *unoui_add_canvas(unoui_window *w, int x, int y, int ww, int h,
                               unoui_canvas *c)
{ unoui_widget *d = push(w, UI_CANVAS, x, y, ww, h, 0); d->canvas = c; return d; }

unoui_widget *unoui_add_edit(unoui_window *w, int x, int y, int ww, unoui_text *t)
{ unoui_widget *d = push(w, UI_FIELD, x, y, ww, ui_field_h(), 0); d->edit = t; return d; }

unoui_widget *unoui_add_textarea(unoui_window *w, int x, int y, int ww, int h,
                                 unoui_text *t)
{ unoui_widget *d = push(w, UI_TEXTAREA, x, y, ww, h, 0); d->edit = t; return d; }

unoui_widget *unoui_add_hscroll(unoui_window *w, int x, int y, int ww, int v, int vm)
{ unoui_widget *d = push(w, UI_HSCROLL, x, y, ww, 14, 0);
  d->value = v; d->vmax = vm; return d; }

unoui_widget *unoui_add_slider(unoui_window *w, int x, int y, int ww,
                               int vmin, int vmax, int v)
{ unoui_widget *d = push(w, UI_SLIDER, x, y, ww, ui_field_h(), 0);
  d->vmin = vmin; d->vmax = vmax; d->value = v; return d; }

unoui_widget *unoui_add_spinner(unoui_window *w, int x, int y, int ww,
                                int vmin, int vmax, int v)
{ unoui_widget *d = push(w, UI_SPINNER, x, y, ww, ui_field_h(), 0);
  d->vmin = vmin; d->vmax = vmax; d->value = v; return d; }

unoui_widget *unoui_add_dropdown(unoui_window *w, int x, int y, int ww,
                                 const char **items, int n, int sel)
{ unoui_widget *d = push(w, UI_DROPDOWN, x, y, ww, ui_field_h(), 0);
  d->items = items; d->nitems = n; d->sel = sel; return d; }

unoui_widget *unoui_add_tabs(unoui_window *w, int x, int y, int ww,
                             const char **items, int n, int sel)
{ unoui_widget *d = push(w, UI_TABS, x, y, ww, UI_TAB_H, 0);
  d->items = items; d->nitems = n; d->sel = sel; return d; }

unoui_widget *unoui_add_menubar(unoui_window *w, const unoui_menu *menus, int n)
{ unoui_widget *d = push(w, UI_MENUBAR, 0, 0, 0, UI_MENUBAR_H, 0);
  d->menus = menus; d->nmenus = n; return d; }

unoui_widget *unoui_add_mdi(unoui_window *w, int x, int y, int ww, int hh,
                            unoui_mdi *m)
{ unoui_widget *d = push(w, UI_MDI, x, y, ww, hh, 0);
  d->mdi = m; return d; }

/* ---- editable text model ------------------------------------------------- */
void unoui_text_init(unoui_text *t, char *buf, int cap, int multiline)
{
    int n = 0, again = (t->buf == buf);
    while (buf[n] && n < cap - 1) n++;
    t->buf = buf; t->cap = cap; t->len = n;
    t->multiline = multiline;
    /* RE-BINDING THE SAME BUFFER KEEPS THE CARET.
     *
     * Because a window BUILDER is a function that runs many times - on a tab
     * switch, a refresh, a font change, a lease arriving - and this was written
     * as though it ran once. Every rebuild slammed the caret to the end of the
     * text, so typing into a field in a window that rebuilt underneath you
     * jumped the cursor away mid-word. Reported twice: once as the WiFi
     * password field (patched there, in the app, which fixed one field out of
     * five), and again afterwards for the others - Files' Name box, the
     * Editor's Find and Replace boxes, the installer's confirmation box.
     *
     * Fixing it in each builder means every FUTURE builder gets it wrong too.
     * A caller that really does want the caret reset is asking to REPLACE the
     * contents, and that is what unoui_text_set() is for. */
    if (!again) { t->caret = t->sel = n; t->scroll_x = t->scroll_y = 0; }
    else {
        if (t->caret > n) t->caret = n;
        if (t->sel   > n) t->sel   = n;
        if (t->caret < 0) t->caret = 0;
        if (t->sel   < 0) t->sel   = 0;
    }
}

void unoui_text_secret(unoui_text *t, int mask_char)
{
    if (!t) return;
    t->secret = mask_char;
    t->revealed = 0;
    if (mask_char) t->multiline = 0;    /* a masked paragraph is not a thing */
}

void unoui_text_show(unoui_text *t, int on)
{ if (t) t->revealed = t->secret ? (on ? 1 : 0) : 0; }

void unoui_text_set(unoui_text *t, const char *s)
{
    int n = 0; while (s[n] && n < t->cap - 1) { t->buf[n] = s[n]; n++; }
    t->buf[n] = 0; t->len = n; t->caret = t->sel = n;
    t->scroll_x = t->scroll_y = 0;
}

/* ------------------------------------------------------ drawing helpers ---- */

void ui_px(int x, int y, fb_px c)
{
    fb_pixel(x, y, c);          /* clip-window aware (see fb_set_clip) */
}

static int chan(fb_px c, int shift) { return (int)((c >> shift) & 0xFF); }

static fb_px mix(fb_px a, fb_px b, int num, int den)
{
    int r = (chan(a,0)  * (den - num) + chan(b,0)  * num) / den;
    int g = (chan(a,8)  * (den - num) + chan(b,8)  * num) / den;
    int bl= (chan(a,16) * (den - num) + chan(b,16) * num) / den;
    return FB_RGB(r, g, bl);
}

/* 4x4 ordered (Bayer) dither matrix, 0..15 */
static const int bayer4[4][4] = {
    {  0,  8,  2, 10 }, { 12,  4, 14,  6 },
    {  3, 11,  1,  9 }, { 15,  7, 13,  5 }
};

void ui_stipple(int x, int y, int w, int h, fb_px a, fb_px b, int density)
{
    int i, j;                       /* density 0..16: fraction that becomes b */
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            ui_px(x + i, y + j,
                  bayer4[(y + j) & 3][(x + i) & 3] < density ? b : a);
}

void ui_shade(int x, int y, int w, int h, const unoui_theme *t,
              fb_px a, fb_px b, int shade)
{
    /* shade 0..UI_SHADES-1 maps dark(a)..light(b) */
    int num = shade * 16 / (UI_SHADES - 1);          /* 0..16 */
    if (t->m.depth == UNOUI_DEPTH_1) {
        ui_stipple(x, y, w, h, a, b, num);           /* dither between a and b */
    } else if (t->m.depth == UNOUI_DEPTH_4) {
        /* coarse: snap to 4 steps, then a light dither to hide banding */
        int q = (num + 2) / 4 * 4;
        ui_stipple(x, y, w, h, mix(a, b, q, 16), mix(a, b, q + 4 > 16 ? 16 : q + 4, 16),
                   (num - q) * 4);
    } else {
        fb_fill_rect(x, y, w, h, mix(a, b, num, 16));
    }
}

unoui_rect ui_bevel(unoui_rect r, const unoui_theme *th, int thick, int lifted)
{
    int i;
    fb_px tl = lifted >= 0 ? th->pal.light  : th->pal.shadow;  /* top-left   */
    fb_px br = lifted >= 0 ? th->pal.shadow : th->pal.light;   /* bot-right  */
    for (i = 0; i < thick; i++) {
        fb_hline(r.x + i, r.y + i, r.w - 2 * i, tl);
        fb_vline(r.x + i, r.y + i, r.h - 2 * i, tl);
        fb_hline(r.x + i, r.y + r.h - 1 - i, r.w - 2 * i, br);
        fb_vline(r.x + r.w - 1 - i, r.y + i, r.h - 2 * i, br);
    }
    r.x += thick; r.y += thick; r.w -= 2 * thick; r.h -= 2 * thick;
    return r;
}

/* corner-clip table: how many px to skip on each row near a rounded corner */
static int corner_inset(int row, int radius)
{
    /* simple quarter-circle-ish staircase */
    static const int r2[] = { 2, 1, 1, 0, 0 };
    static const int r3[] = { 3, 2, 1, 1, 0 };
    if (radius <= 0) return 0;
    if (radius == 2 && row < 2) return r2[row > 4 ? 4 : row] ? r2[row] : 0;
    if (radius >= 3 && row < 3) return r3[row > 4 ? 4 : row];
    if (radius == 2 && row < 5) return r2[row];
    return 0;
}

void ui_round_fill(unoui_rect r, int radius, fb_px c)
{
    int row;
    for (row = 0; row < r.h; row++) {
        int top = row, bot = r.h - 1 - row;
        int ins = corner_inset(top < bot ? top : bot, radius);
        fb_hline(r.x + ins, r.y + row, r.w - 2 * ins, c);
    }
}

void ui_round_frame(unoui_rect r, int radius, fb_px c)
{
    int row;
    for (row = 0; row < r.h; row++) {
        int top = row, bot = r.h - 1 - row;
        int near = top < bot ? top : bot;
        int ins = corner_inset(near, radius);
        if (row == 0 || row == r.h - 1 || near < radius) {
            fb_hline(r.x + ins, r.y + row, r.w - 2 * ins, c);
        }
        /* side rails */
        if (row >= radius && row < r.h - radius) {
            ui_px(r.x, r.y + row, c);
            ui_px(r.x + r.w - 1, r.y + row, c);
        } else {
            ui_px(r.x + ins, r.y + row, c);
            ui_px(r.x + r.w - 1 - ins, r.y + row, c);
        }
    }
}

void ui_text_in(unoui_rect r, const char *s, fb_px fg, long bg, int center)
{
    int tw = fb_text_w(s);
    int tx = center ? r.x + (r.w - tw) / 2 : r.x + 4;
    int ty = r.y + (r.h - fb_text_h()) / 2;
    fb_text(tx, ty, s, fg, bg);
}

/* Canonical content origin from theme metrics - the single source of truth so
 * every window painter and hit-testing agree on where widgets live. */
void unoui_content_origin(const unoui_theme *t, const unoui_window *w,
                          int *ox, int *oy)
{
    if (w->flags & UI_WIN_BARE) {   /* no frame/titlebar: widgets sit at origin */
        *ox = w->r.x; *oy = w->r.y; return;
    }
    *ox = w->r.x + t->m.frame_w + t->m.pad;
    *oy = w->r.y + t->m.title_h + t->m.pad;
    /* A SCROLLED window moves its whole content up. Doing it HERE is what makes
     * scrolling honest: the painter, the hit test, the caret reveal and the
     * layout audit all ask this function where the content starts, so none of
     * them can disagree about where a widget is. */
    if (w->flags & UI_WIN_VSCROLL) *oy -= w->scroll_y;
}

/* ---- scrolling window content (see unoui.h) ------------------------------- */
/* the content box a scrolling window's widgets are seen through */
static unoui_rect win_view(const unoui_theme *t, const unoui_window *w)
{
    unoui_rect v;
    if (w->flags & UI_WIN_BARE) { v = w->r; return v; }
    v.x = w->r.x + t->m.frame_w + t->m.pad;
    v.y = w->r.y + t->m.title_h + t->m.pad;
    v.w = w->r.w - 2 * (t->m.frame_w + t->m.pad);
    v.h = w->r.h - t->m.title_h - t->m.pad - t->m.frame_w;
    return v;
}

int unoui_win_scroll_max(const unoui_theme *t, const unoui_window *w)
{
    int over;
    if (!t || !w || !(w->flags & UI_WIN_VSCROLL)) return 0;
    over = w->content_h - win_view(t, w).h;
    return over > 0 ? over : 0;
}

void unoui_win_scroll_to(const unoui_theme *t, unoui_window *w, int y)
{
    int mx = unoui_win_scroll_max(t, w);
    if (!w) return;
    if (y > mx) y = mx;
    if (y < 0)  y = 0;
    w->scroll_y = y;
}

unoui_rect unoui_win_bar(const unoui_theme *t, const unoui_window *w)
{
    unoui_rect z; z.x = z.y = z.w = z.h = 0;
    if (!t || !w || !(w->flags & UI_WIN_VSCROLL)) return z;
    if (unoui_win_scroll_max(t, w) <= 0) return z;      /* it all fits */
    { unoui_rect v = win_view(t, w);
      if (v.w <= 3 * UI_LIST_BAR_W) return z;           /* too narrow to spare it */
      z.x = v.x + v.w - UI_LIST_BAR_W; z.y = v.y;
      z.w = UI_LIST_BAR_W;             z.h = v.h;
      return z; }
}

/* ------------------------------------------------- default painters -------- *
 * The house UnoDOS look: a clean single-bevel style. Themes reuse or override
 * any of these. They reference ONLY theme->pal / theme->m, never raw colours.  */

static void d_desktop(const unoui_theme *t, int W, int H)
{
    if (t->pal.desktop2 != t->pal.desktop)
        ui_stipple(0, 0, W, H, t->pal.desktop, t->pal.desktop2, 8);
    else
        fb_fill_rect(0, 0, W, H, t->pal.desktop);
}

static void d_window(const unoui_theme *t, unoui_window *win)
{
    unoui_rect r = win->r;
    int fw = t->m.frame_w, th = t->m.title_h;
    if (t->m.shadow_off) {
        ui_stipple(r.x + t->m.shadow_off, r.y + r.h, r.w, t->m.shadow_off,
                   t->pal.dark, t->pal.dark, 16);
        ui_stipple(r.x + r.w, r.y + t->m.shadow_off, t->m.shadow_off, r.h,
                   t->pal.dark, t->pal.dark, 16);
    }
    /* outer frame */
    { int i; for (i = 0; i < fw; i++)
        fb_frame_rect(r.x + i, r.y + i, r.w - 2 * i, r.h - 2 * i, t->pal.win_frame); }
    /* content fill (below the title bar) */
    fb_fill_rect(r.x + fw, r.y + th, r.w - 2 * fw, r.h - th - fw, t->pal.win_bg);
    unoui_content_origin(t, win, &win->content_x, &win->content_y);
}

static void d_titlebar(const unoui_theme *t, const unoui_window *win)
{
    unoui_rect r = win->r;
    int fw = t->m.frame_w, th = t->m.title_h;
    fb_px bg = win->active ? t->pal.title_bg : t->pal.title_bg_in;
    fb_px fg = win->active ? t->pal.title_fg : t->pal.title_fg_in;
    unoui_rect bar = { r.x + fw, r.y + fw, r.w - 2 * fw, th - fw };
    fb_fill_rect(bar.x, bar.y, bar.w, bar.h, bg);
    fb_hline(bar.x, r.y + th - 1, bar.w, t->pal.win_frame);
    if (t->m.closebox) {
        unoui_rect cb = unoui_closebox_rect(t, r);
        ui_bevel(cb, t, 1, 1);
        /* the title text yields to whichever side the box took */
        if (t->m.closeright) bar.w -= t->m.closebox + 8;
        else { bar.x += t->m.closebox + 8; bar.w -= t->m.closebox + 8; }
    }
    ui_text_in(bar, win->title, fg, -1, t->m.title_center);
}

static void d_label(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    fb_text(r.x, r.y, s, (f & UI_F_DISABLED) ? t->pal.text_dim : t->pal.text, -1);
}

static void d_button(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    int press = (f & UI_F_PRESSED) != 0;
    unoui_rect in;
    if (f & UI_F_DEFAULT) {                       /* default ring */
        fb_frame_rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4, t->pal.dark);
        fb_frame_rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6, t->pal.dark);
    }
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.face);
    /* crisp 1px outer edge so buttons read as distinct raised controls, then
     * the bevel inside it (skip the hard edge on 1-bit themes - the dither
     * bevel carries the shape there). */
    if (t->m.depth != UNOUI_DEPTH_1) {
        fb_frame_rect(r.x, r.y, r.w, r.h, t->pal.dark);
        { unoui_rect ir = { r.x + 1, r.y + 1, r.w - 2, r.h - 2 };
          ui_bevel(ir, t, t->m.bevel ? t->m.bevel : 1, press ? -1 : 1); }
    } else {
        ui_bevel(r, t, t->m.bevel ? t->m.bevel : 1, press ? -1 : 1);
    }
    in = r; (void)in;
    if (press) { r.x++; r.y++; }
    ui_text_in(r, s, (f & UI_F_DISABLED) ? t->pal.text_dim : t->pal.face_text,
               -1, 1);
}

static void d_check(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    int by = r.y + (r.h - 12) / 2;                 /* box centred in the row */
    unoui_rect box = { r.x, by, 12, 12 };
    fb_fill_rect(box.x, box.y, 12, 12, t->pal.field_bg);
    ui_bevel(box, t, 1, -1);
    if (f & UI_F_CHECKED) {                        /* an X */
        int i; for (i = 2; i < 10; i++) {
            ui_px(box.x + i, box.y + i, t->pal.text);
            ui_px(box.x + 11 - i, box.y + i, t->pal.text);
        }
    }
    fb_text(r.x + 18, r.y + (r.h - fb_text_h()) / 2, s,
            (f & UI_F_DISABLED) ? t->pal.text_dim : t->pal.text, -1);
}

static void d_radio(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    /* small filled-circle radio drawn in a 12x12 cell, centred in the row */
    int cx = r.x + 6, cy = r.y + (r.h - 12) / 2 + 6, yy, xx;
    for (yy = -5; yy <= 5; yy++)
      for (xx = -5; xx <= 5; xx++) {
        int d = xx*xx + yy*yy;
        if (d <= 25 && d > 16) ui_px(cx+xx, cy+yy, t->pal.dark);
        else if (d <= 16)      ui_px(cx+xx, cy+yy, t->pal.field_bg);
      }
    if (f & UI_F_CHECKED)
      for (yy = -2; yy <= 2; yy++)
        for (xx = -2; xx <= 2; xx++)
          if (xx*xx + yy*yy <= 5) ui_px(cx+xx, cy+yy, t->pal.text);
    fb_text(r.x + 18, r.y + (r.h - fb_text_h()) / 2, s,
            (f & UI_F_DISABLED) ? t->pal.text_dim : t->pal.text, -1);
}

/* ---- editable-text geometry + drawing (shared by field + textarea) ------- */

unoui_rect ui_edit_inner(unoui_rect r, const unoui_theme *th)
{
    (void)th;
    { unoui_rect in = { r.x + 1, r.y + 1, r.w - 2, r.h - 2 }; return in; }
}

static void line_span(const unoui_text *t, int line, int *s, int *e)
{
    int i = 0, cur = 0; *s = 0;
    while (cur < line && i < t->len) { if (t->buf[i] == '\n') { cur++; *s = i + 1; } i++; }
    if (cur < line) *s = t->len;
    *e = *s; while (*e < t->len && t->buf[*e] != '\n') (*e)++;
}

static void idx_linecol(const unoui_text *t, int idx, int *line, int *col)
{
    int i, l = 0, c = 0;
    for (i = 0; i < idx && i < t->len; i++) {
        if (t->buf[i] == '\n') { l++; c = 0; } else c++;
    }
    *line = l; *col = c;
}

static int text_lines(const unoui_text *t)
{
    int i, n = 1; for (i = 0; i < t->len; i++) if (t->buf[i] == '\n') n++; return n;
}

void ui_text_caret_xy(unoui_rect in, const unoui_text *t, int idx, int *cx, int *cy)
{
    int line, col, s, e; idx_linecol(t, idx, &line, &col);
    line_span(t, line, &s, &e);
    *cx = in.x + 3 + t_seg_w(t, s, s + col) - t->scroll_x;
    *cy = t->multiline ? in.y + 2 + line * UI_LINE_H - t->scroll_y
                       : in.y + (in.h - fb_text_h()) / 2;
}

int ui_text_index_at(unoui_rect in, const unoui_text *t, int px, int py)
{
    int line = 0, s, e, i, prev, want;
    if (t->multiline) {
        line = (py - (in.y + 2) + t->scroll_y) / UI_LINE_H;
        if (line < 0) line = 0;
        if (line > text_lines(t) - 1) line = text_lines(t) - 1;
    }
    line_span(t, line, &s, &e);
    /* walk the line's glyphs; snap to whichever gap is nearest the pointer.
     *
     * Each gap is measured as a PREFIX from the start of the line - the same
     * measurement ui_text_caret_xy makes and the same one the painter's pen
     * performs - not as a running sum of per-glyph widths.  Summing per glyph
     * was wrong on any proportional font: fb_text_w rounds its 26.6 pen to
     * whole pixels once per call, so a per-character sum banks up to half a
     * pixel of rounding EVERY character and throws away the kerning between
     * each pair.  Over a dozen characters that is several pixels, and the
     * click landed a glyph or two from the one under the pointer - reported as
     * the caret jumping to the wrong spot while typing a WiFi password, but
     * it was every text field in the OS. */
    want = px - (in.x + 3) + t->scroll_x;
    prev = 0;
    for (i = s; i < e; i++) {
        int adv = t_seg_w(t, s, i + 1);         /* pen after glyph i */
        if (want < (prev + adv) / 2) return i;  /* nearer this glyph's left gap */
        prev = adv;
    }
    return e;
}

void ui_text_reveal(unoui_rect in, unoui_text *t)
{
    int line, col, s, e, cpx, vis_w = in.w - 6, vis_h = in.h - 4;
    idx_linecol(t, t->caret, &line, &col);
    line_span(t, line, &s, &e);
    cpx = 3 + t_seg_w(t, s, s + col);
    if (cpx - t->scroll_x < 0)         t->scroll_x = cpx;
    if (cpx - t->scroll_x > vis_w)     t->scroll_x = cpx - vis_w;
    if (t->scroll_x < 0) t->scroll_x = 0;
    if (t->multiline) {
        int top = 2 + line * UI_LINE_H;
        if (top - t->scroll_y < 0)                 t->scroll_y = top;
        if (top + UI_LINE_H - t->scroll_y > vis_h) t->scroll_y = top + UI_LINE_H - vis_h;
        if (t->scroll_y < 0) t->scroll_y = 0;
    }
}

static void clamp_fill(unoui_rect clip, int x, int y, int w, int h, fb_px c)
{
    if (x < clip.x) { w -= clip.x - x; x = clip.x; }
    if (y < clip.y) { h -= clip.y - y; y = clip.y; }
    if (x + w > clip.x + clip.w) w = clip.x + clip.w - x;
    if (y + h > clip.y + clip.h) h = clip.y + clip.h - y;
    if (w > 0 && h > 0) fb_fill_rect(x, y, w, h, c);
}

/* ---- the reveal eye -------------------------------------------------------
 * A square at the field's right-hand end, as tall as the text. Its rect is
 * computed HERE and used by both the painter and the hit test, so a click on
 * the eye can never be off by a pixel from the eye that was drawn. Zero-sized
 * when the field is not secret, or when the field is too narrow to give up the
 * room (a 40 px field with an eye in it is not a field). */
unoui_rect ui_edit_eye_rect(unoui_rect in, const unoui_text *t)
{
    unoui_rect z; int side;
    z.x = z.y = z.w = z.h = 0;
    if (!t || !t->secret) return z;
    side = fb_text_h() + 2;
    if (side < 9) side = 9;
    if (in.w < side + 40) return z;            /* no room: no eye */
    z.w = side; z.h = side;
    z.x = in.x + in.w - side - 2;
    z.y = in.y + (in.h - side) / 2;
    return z;
}

/* The part of a field's inner rect the TEXT gets: everything left of the eye.
 * Every measurement the toolkit makes about an editable field is against this
 * rect, so a secret field scrolls and hit-tests inside its own text area rather
 * than underneath the eye. */
unoui_rect ui_edit_text_rect(unoui_rect in, const unoui_text *t)
{
    unoui_rect e = ui_edit_eye_rect(in, t);
    if (e.w) in.w = e.x - in.x - 2;
    return in;
}

/* A lens outline with a pupil; a stroke through it when the text is showing,
 * which is the "click to hide again" state. Deliberately drawn from ui_px and
 * spans rather than a bitmap, so it follows the font size and the theme's
 * colours on every port. */
static void draw_eye(unoui_rect e, const unoui_theme *th, int showing)
{
    int cx = e.x + e.w / 2, cy = e.y + e.h / 2;
    int rx = e.w / 2 - 1, ry = e.h / 4;
    fb_px c = showing ? th->pal.accent : th->pal.text_dim;
    int x;
    if (rx < 2 || ry < 1) return;
    /* the lens: two arcs meeting at the corners, as y = +-ry*(1-(x/rx)^2) */
    for (x = -rx; x <= rx; x++) {
        int dy = (ry * (rx * rx - x * x) + rx * rx / 2) / (rx * rx);
        ui_px(cx + x, cy - dy, c);
        ui_px(cx + x, cy + dy, c);
    }
    /* the pupil */
    { int r = ry > 2 ? 2 : 1, dx, dy;
      for (dy = -r; dy <= r; dy++)
          for (dx = -r; dx <= r; dx++)
              if (dx * dx + dy * dy <= r * r) ui_px(cx + dx, cy + dy, c); }
    if (showing) {                            /* struck through = it is visible */
        int i, n = e.w;
        for (i = 0; i < n; i++) ui_px(e.x + i, e.y + e.h - 1 - i, c);
    }
}

static void draw_edit_text(unoui_rect box, const unoui_text *t,
                           const unoui_theme *th, int focused, int caret_on)
{
    unoui_rect in = ui_edit_text_rect(box, t);
    int selA = t->sel < t->caret ? t->sel : t->caret;
    int selB = t->sel < t->caret ? t->caret : t->sel;
    int nlines = text_lines(t), line, fh = fb_text_h(), lh = UI_LINE_H;
    int scx, scy, scw, sch;                        /* clip save */
    fb_get_clip(&scx, &scy, &scw, &sch);
    /* confine text (which may scroll past the box) to the inner rect, inside
     * whatever window clip is already active */
    { int x0 = in.x > scx ? in.x : scx, y0 = in.y > scy ? in.y : scy;
      int x1 = in.x + in.w < scx + scw ? in.x + in.w : scx + scw;
      int y1 = in.y + in.h < scy + sch ? in.y + in.h : scy + sch;
      fb_set_clip(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0); }
    for (line = 0; line < nlines; line++) {
        int s, e, y, x0;
        line_span(t, line, &s, &e);
        y = t->multiline ? in.y + 2 + line * lh - t->scroll_y
                         : in.y + (in.h - fh) / 2;
        if (y + fh < in.y || y > in.y + in.h) continue;         /* vertical clip */
        x0 = in.x + 3 - t->scroll_x;
        if (focused && selB > selA) {                           /* selection bg */
            int a = s > selA ? s : selA, b = e < selB ? e : selB;
            if (b > a) {
                int sx = x0 + t_seg_w(t, s, a);
                clamp_fill(in, sx, y - 1, t_seg_w(t, a, b), lh, th->pal.accent);
            }
        }
        /* draw up to three runs so selection recolours without breaking the
         * glyph flow: [s,a) normal, [a,b) selected, [b,e) normal */
        { int a = focused && selB > selA ? (selA > s ? (selA < e ? selA : e) : s) : e;
          int b = focused && selB > selA ? (selB > s ? (selB < e ? selB : e) : s) : e;
          int x = x0;
          x = t_seg_draw(t, s, a, x, y, th->pal.field_text, -1);
          x = t_seg_draw(t, a, b, x, y, th->pal.accent_text, -1);
          (void)t_seg_draw(t, b, e, x, y, th->pal.field_text, -1); }
    }
    if (focused && caret_on) {
        int cx, cy; ui_text_caret_xy(in, t, t->caret, &cx, &cy);
        if (cx >= in.x && cx <= in.x + in.w)
            fb_vline(cx, cy - 1, fh + 1, th->pal.field_text);
    }
    fb_set_clip(scx, scy, scw, sch);
    { unoui_rect eye = ui_edit_eye_rect(box, t);
      if (eye.w) draw_eye(eye, th, t->revealed); }
}

/* public wrapper so custom themes can reuse the exact default text painter
 * (selection runs, fractional-pen positioning, clip) inside their own field
 * chrome instead of reimplementing it. */
void ui_draw_edit_text(unoui_rect in, const unoui_text *t,
                       const unoui_theme *th, int focused, int caret_on)
{
    draw_edit_text(in, t, th, focused, caret_on);
}

static void d_field(const unoui_theme *t, unoui_rect r, const char *s,
                    unoui_text *ed, int f)
{
    unoui_rect in;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.field_bg);
    ui_bevel(r, t, 1, -1);
    in = ui_edit_inner(r, t);
    if (ed) {
        draw_edit_text(in, ed, t, (f & UI_F_FOCUS) != 0, (f & UI_F_CARET) != 0);
    } else {                                                    /* static text */
        fb_text(in.x + 2, in.y + (in.h - fb_text_h()) / 2, s ? s : "",
                t->pal.field_text, -1);
        if (f & UI_F_CARET)
            fb_vline(in.x + 2 + fb_text_w(s ? s : ""), in.y + 2, in.h - 4,
                     t->pal.field_text);
    }
}

static void d_textarea(const unoui_theme *t, unoui_rect r, unoui_text *ed, int f)
{
    unoui_rect in;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.field_bg);
    ui_bevel(r, t, 1, -1);
    in = ui_edit_inner(r, t);
    if (ed) draw_edit_text(in, ed, t, (f & UI_F_FOCUS) != 0, (f & UI_F_CARET) != 0);
}

/* UI_BUSY: eight dots round a ring, brightest at `phase` and fading behind it.
 * A ring rather than a bar because a bar implies a proportion it does not have,
 * and eight because the trail has to be readable at 9 px as well as at 32. */
static void d_busy(const unoui_theme *t, unoui_rect r, int phase)
{
    /* unit-circle offsets x1000, so no float and no trig table */
    static const short CX[UI_BUSY_DOTS] = {    0,  707, 1000,  707,    0, -707, -1000, -707 };
    static const short CY[UI_BUSY_DOTS] = {-1000, -707,    0,  707, 1000,  707,     0, -707 };
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    int rad = (r.w < r.h ? r.w : r.h) / 2 - 2;
    int dot = rad / 4, i;
    if (rad < 3) return;
    if (dot < 1) dot = 1;
    for (i = 0; i < UI_BUSY_DOTS; i++) {
        /* how far BEHIND the head this dot is: 0 = the head itself */
        int back = (i - phase) % UI_BUSY_DOTS;
        int px, py, dx, dy;
        fb_px c;
        if (back < 0) back += UI_BUSY_DOTS;
        /* head and the two behind it are the accent; the rest is the dim trail */
        c = (back == 0 || back >= UI_BUSY_DOTS - 2) ? t->pal.accent : t->pal.text_dim;
        px = cx + CX[i] * rad / 1000;
        py = cy + CY[i] * rad / 1000;
        for (dy = -dot; dy <= dot; dy++)
            for (dx = -dot; dx <= dot; dx++)
                if (dx * dx + dy * dy <= dot * dot) ui_px(px + dx, py + dy, c);
    }
}

static void d_progress(const unoui_theme *t, unoui_rect r, int v, int vm)
{
    unoui_rect in;
    int fill;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.field_bg);
    in = ui_bevel(r, t, 1, -1);
    fill = vm > 0 ? in.w * v / vm : 0;
    fb_fill_rect(in.x, in.y, fill, in.h, t->pal.accent);
}

static void d_vscroll(const unoui_theme *t, unoui_rect r, int v, int vm)
{
    int track_h, thumb_h, thumb_y;
    /* up/down arrow boxes */
    unoui_rect up = { r.x, r.y, r.w, r.w }, dn = { r.x, r.y + r.h - r.w, r.w, r.w };
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.face);
    track_h = r.h - 2 * r.w;
    thumb_h = vm > 0 ? (track_h * track_h) / (track_h + vm) : track_h;
    if (thumb_h < 8) thumb_h = 8;
    thumb_y = r.y + r.w + (vm > 0 ? (track_h - thumb_h) * v / vm : 0);
    { unoui_rect th = { r.x + 1, thumb_y, r.w - 2, thumb_h };
      fb_fill_rect(th.x, th.y, th.w, th.h, t->pal.face); ui_bevel(th, t, 1, 1); }
    fb_fill_rect(up.x, up.y, up.w, up.h, t->pal.face); ui_bevel(up, t, 1, 1);
    fb_fill_rect(dn.x, dn.y, dn.w, dn.h, t->pal.face); ui_bevel(dn, t, 1, 1);
    { int i; for (i = 0; i < 4; i++) {            /* triangles */
        fb_hline(up.x + up.w/2 - i, up.y + 4 + i, 2*i+1, t->pal.face_text);
        fb_hline(dn.x + dn.w/2 - (3-i), dn.y + 4 + i, 2*(3-i)+1, t->pal.face_text);
      } }
}

static void d_list(const unoui_theme *t, unoui_rect r, const char **it, int n,
                   int sel, int top)
{
    int i, row = ui_row_h(), y;
    unoui_rect in;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.field_bg);
    in = ui_bevel(r, t, 1, -1);
    for (i = top, y = in.y + 2; i < n && y + row <= in.y + in.h; i++, y += row) {
        fb_px fg = t->pal.field_text;
        if (i == sel) {
            fb_fill_rect(in.x, y - 1, in.w, row, t->pal.accent);
            fg = t->pal.accent_text;
        }
        fb_text(in.x + 3, y + (row - 1 - fb_text_h()) / 2, it[i], fg, -1);
    }
}

static void d_group(const unoui_theme *t, unoui_rect r, const char *s)
{
    int fh = fb_text_h();
    fb_frame_rect(r.x, r.y + fh / 2, r.w, r.h - fh / 2, t->pal.shadow);
    fb_frame_rect(r.x + 1, r.y + fh / 2 + 1, r.w, r.h - fh / 2, t->pal.light);
    if (s && *s) {
        int tw = fb_text_w(s);
        fb_fill_rect(r.x + 8, r.y, tw + 6, fh, t->pal.win_bg);
        fb_text(r.x + 11, r.y, s, t->pal.text, -1);
    }
}

static void d_sep(const unoui_theme *t, unoui_rect r)
{
    fb_hline(r.x, r.y, r.w, t->pal.shadow);
    fb_hline(r.x, r.y + 1, r.w, t->pal.light);
}

static void d_icon(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    int fh = fb_text_h();
    unoui_rect g = { r.x + (r.w - 32) / 2, r.y, 32, 28 };
    fb_fill_rect(g.x, g.y, g.w, g.h, t->pal.face);
    ui_bevel(g, t, 1, 1);
    /* a little folder/doc glyph */
    fb_fill_rect(g.x + 6, g.y + 8, 20, 14, t->pal.accent);
    if (f & UI_F_FOCUS)                                   /* selected label bg */
        fb_fill_rect(r.x, r.y + 30, r.w, fh + 2, t->pal.accent);
    ui_text_in((unoui_rect){ r.x, r.y + 31, r.w, fh }, s,
               (f & UI_F_FOCUS) ? t->pal.accent_text : t->pal.text, -1, 1);
}

static void ui_itoa(int v, char *out)
{
    char tmp[16]; int n = 0, k = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)(-(long)v) : (unsigned)v;
    if (!u) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    if (neg) out[k++] = '-';
    while (n) out[k++] = tmp[--n];
    out[k] = 0;
}

static void d_hscroll(const unoui_theme *t, unoui_rect r, int v, int vm)
{
    int bw = r.h, track_w = r.w - 2 * bw, thumb_w, thumb_x, i;
    unoui_rect lf = { r.x, r.y, bw, r.h }, rt = { r.x + r.w - bw, r.y, bw, r.h };
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.face);
    thumb_w = vm > 0 ? (track_w * track_w) / (track_w + vm) : track_w;
    if (thumb_w < 8) thumb_w = 8;
    thumb_x = r.x + bw + (vm > 0 ? (track_w - thumb_w) * v / vm : 0);
    { unoui_rect th = { thumb_x, r.y + 1, thumb_w, r.h - 2 };
      fb_fill_rect(th.x, th.y, th.w, th.h, t->pal.face); ui_bevel(th, t, 1, 1); }
    fb_fill_rect(lf.x, lf.y, lf.w, lf.h, t->pal.face); ui_bevel(lf, t, 1, 1);
    fb_fill_rect(rt.x, rt.y, rt.w, rt.h, t->pal.face); ui_bevel(rt, t, 1, 1);
    for (i = 0; i < 4; i++) {
        fb_vline(lf.x + 4 + i,        lf.y + r.h/2 - i,     2*i+1,     t->pal.face_text);
        fb_vline(rt.x + bw - 5 - i,   rt.y + r.h/2 - i,     2*i+1,     t->pal.face_text);
    }
}

static void d_slider(const unoui_theme *t, unoui_rect r, int v, int vmin, int vmax, int f)
{
    int kw = 9, range = vmax - vmin, span = r.w - 6 - kw, kx;
    if (range < 1) range = 1;
    kx = r.x + 3 + span * (v - vmin) / range;
    fb_fill_rect(r.x + 3, r.y + r.h/2 - 1, r.w - 6, 2, t->pal.shadow);
    fb_hline(r.x + 3, r.y + r.h/2 + 1, r.w - 6, t->pal.light);
    { unoui_rect k = { kx, r.y + 2, kw, r.h - 4 };
      fb_fill_rect(k.x, k.y, k.w, k.h, t->pal.face); ui_bevel(k, t, 1, 1); }
    if (f & UI_F_FOCUS) fb_frame_rect(r.x, r.y, r.w, r.h, t->pal.accent);
}

static void d_spinner(const unoui_theme *t, unoui_rect r, int v, int f)
{
    int bw = 12, i; char num[16]; ui_itoa(v, num);
    { unoui_rect box = { r.x, r.y, r.w - bw, r.h };
      fb_fill_rect(box.x, box.y, box.w, box.h, t->pal.field_bg); ui_bevel(box, t, 1, -1);
      fb_text(box.x + 3, box.y + (box.h - fb_text_h())/2, num, t->pal.field_text, -1);
      if (f & UI_F_FOCUS) fb_frame_rect(box.x, box.y, box.w, box.h, t->pal.accent); }
    { unoui_rect up = { r.x + r.w - bw, r.y, bw, r.h/2 };
      unoui_rect dn = { r.x + r.w - bw, r.y + r.h/2, bw, r.h - r.h/2 };
      fb_fill_rect(up.x, up.y, up.w, up.h, t->pal.face); ui_bevel(up, t, 1, 1);
      fb_fill_rect(dn.x, dn.y, dn.w, dn.h, t->pal.face); ui_bevel(dn, t, 1, 1);
      for (i = 0; i < 3; i++) {
          fb_hline(up.x + bw/2 - i,     up.y + up.h/2 - 1 + i, 2*i+1,     t->pal.face_text);
          fb_hline(dn.x + bw/2 - (2-i), dn.y + 1 + i,          2*(2-i)+1, t->pal.face_text);
      } }
}

static void d_dropdown(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    int bw = 14, i;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.field_bg); ui_bevel(r, t, 1, -1);
    fb_text(r.x + 3, r.y + (r.h - fb_text_h())/2, s ? s : "", t->pal.field_text, -1);
    { unoui_rect b = { r.x + r.w - bw, r.y + 1, bw - 1, r.h - 2 };
      fb_fill_rect(b.x, b.y, b.w, b.h, t->pal.face); ui_bevel(b, t, 1, 1);
      for (i = 0; i < 4; i++)
          fb_hline(b.x + b.w/2 - (3 - i), b.y + b.h/2 - 2 + i, 2*(3-i)+1, t->pal.face_text); }
    if (f & UI_F_FOCUS) fb_frame_rect(r.x, r.y, r.w, r.h, t->pal.accent);
}

static void d_tabs(const unoui_theme *t, unoui_rect r, const char **it, int n, int sel, int f)
{
    int i, x = r.x; (void)f;
    fb_hline(r.x, r.y + r.h - 1, r.w, t->pal.dark);            /* baseline */
    for (i = 0; i < n; i++) {
        int tw = fb_text_w(it[i]) + 16, top = (i == sel) ? r.y : r.y + 2;
        unoui_rect tab = { x, top, tw, r.y + r.h - top - (i == sel ? 0 : 1) };
        fb_fill_rect(tab.x, tab.y, tab.w, tab.h, (i == sel) ? t->pal.win_bg : t->pal.face);
        fb_hline(tab.x, tab.y, tab.w, t->pal.dark);
        fb_vline(tab.x, tab.y, tab.h, t->pal.light);
        fb_vline(tab.x + tab.w - 1, tab.y, tab.h, t->pal.shadow);
        fb_text(tab.x + 8, top + (tab.h - fb_text_h())/2, it[i],
                (i == sel) ? t->pal.text : t->pal.text_dim, -1);
        x += tw;
    }
}

/* index of the menubar title under px (or -1); *tx gets its left x */
int unoui_menubar_index_at(const unoui_theme *t, unoui_rect r,
                           const unoui_menu *m, int n, int px, int *tx)
{
    int i, x = r.x + 2;
    (void)t;
    for (i = 0; i < n; i++) {
        int tw = fb_text_w(m[i].title) + 12;
        if (px >= x && px < x + tw) { if (tx) *tx = x; return i; }
        x += tw;
    }
    if (tx) *tx = r.x + 2;
    return -1;
}

static void d_menubar(const unoui_theme *t, unoui_rect r, const unoui_menu *m,
                      int n, int open, int hot)
{
    int i, x = r.x + 2; (void)hot;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.face);
    fb_hline(r.x, r.y + r.h - 1, r.w, t->pal.shadow);
    for (i = 0; i < n; i++) {
        int tw = fb_text_w(m[i].title) + 12;
        if (i == open) {
            fb_fill_rect(x, r.y + 1, tw, r.h - 2, t->pal.accent);
            fb_text(x + 6, r.y + (r.h - fb_text_h())/2, m[i].title, t->pal.accent_text, -1);
        } else {
            fb_text(x + 6, r.y + (r.h - fb_text_h())/2, m[i].title, t->pal.text, -1);
        }
        x += tw;
    }
}

static void d_popup(const unoui_theme *t, unoui_rect r, const char **it, int n, int hot)
{
    int i, row = ui_prow_h(), y, fh = fb_text_h();
    ui_stipple(r.x + 2, r.y + r.h, r.w, 2, t->pal.dark, t->pal.dark, 16);
    ui_stipple(r.x + r.w, r.y + 2, 2, r.h, t->pal.dark, t->pal.dark, 16);
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.win_bg);
    fb_frame_rect(r.x, r.y, r.w, r.h, t->pal.dark);
    for (i = 0, y = r.y + 3; i < n; i++, y += row) {
        fb_px fg = t->pal.text;
        if (i == hot) { fb_fill_rect(r.x + 1, y - 1, r.w - 2, row, t->pal.accent);
                        fg = t->pal.accent_text; }
        fb_text(r.x + 6, y + (row - fh - 1) / 2, it[i], fg, -1);
    }
}

const unoui_draw unoui_default_draw = {
    d_desktop, d_window, d_titlebar, d_button, d_check, d_radio, d_field,
    d_label, d_progress, d_vscroll, d_list, d_group, d_sep, d_icon,
    d_textarea, d_hscroll, d_slider, d_spinner, d_dropdown, d_tabs,
    d_menubar, d_popup, d_busy
};

/* ----------------------------------------------------------- dispatch ------ */

#define PICK(fn) (d->fn ? d->fn : unoui_default_draw.fn)
#define CARET_BLINK 18u

/* absolute screen rect of a widget (menubar spans the content top edge) */
unoui_rect unoui_widget_rect(const unoui_theme *t, const unoui_window *win,
                             const unoui_widget *w)
{
    if (w->kind == UI_MENUBAR) {
        unoui_rect r = { win->r.x + t->m.frame_w, win->r.y + t->m.title_h,
                         win->r.w - 2 * t->m.frame_w, UI_MENUBAR_H };
        return r;
    }
    { int ox, oy; unoui_content_origin(t, win, &ox, &oy);
      /* `dx` is added HERE, which is what makes a shaken widget really move:
       * the painter, the hit test and the layout audit all ask this function
       * where a widget is, so none of them can disagree with the screen. */
      { unoui_rect r = { ox + w->r.x + w->dx, oy + w->r.y, w->r.w, w->r.h };
        return r; } }
}

/* ---- layout audit ---------------------------------------------------------
 * The window content rect is CLIPPED (render_window draws widgets inside it),
 * so a layout that does not fit is not a mess on the desktop - it is silently
 * cut off at the frame, which is worse: the machine looks fine and a button
 * reads "Allow se...".  Every one of those found so far was found by somebody
 * squinting at a screenshot on a 400x300 desktop, one window at a time.
 *
 * This walks a window as BUILT and reports the widgets that will be cut.  Pure
 * geometry against the live font - no drawing, no allocation, no I/O - so the
 * caller can sweep every window in the OS in one pass, at whatever font and UI
 * scale it likes.  pc64 runs it from the debug build (`layout-audit` in
 * DEBUG.CFG); anything else may call it whenever.
 *
 * WHAT IT DELIBERATELY DOES NOT FLAG.  A list, textarea, canvas or MDI is
 * MEANT to hold more than it shows - that is what its scrollbar is for - so
 * only their rects are checked, never their contents.  Anything scrolled
 * horizontally by t->scroll_x is exempt for the same reason. */
static int text_overflows(const unoui_widget *w)
{
    int tw;
    if (!w->text || !w->text[0]) return 0;
    tw = fb_text_w(w->text);
    switch (w->kind) {
    case UI_LABEL:                 return tw > w->r.w;
    /* the painters inset a button's text; a check/radio also spends its box +
     * gap before the label starts (see unoui_add_check) */
    case UI_BUTTON:                return tw + 8  > w->r.w;
    case UI_CHECK: case UI_RADIO:  return tw + 18 > w->r.w;
    case UI_GROUP:                 return tw + 12 > w->r.w;
    case UI_FIELD:                 return w->edit ? 0 : tw + 6 > w->r.w;
    default:                       return 0;
    }
}

/* the width a UI_TABS strip needs, by the same sum lay_tabs makes.  A strip
 * with UI_TF_OVERFLOW or UI_TF_ELASTIC is DESIGNED not to fit (it scrolls, or
 * shares the width out), so only a plain strip can be too small. */
static int tabs_needed_w(const unoui_widget *w)
{
    int i, n = 0, cb;
    if (!w->items || (w->flags & (UI_TF_OVERFLOW | UI_TF_ELASTIC))) return 0;
    cb = (w->flags & UI_TF_CLOSE) ? 12 : 0;
    for (i = 0; i < w->nitems; i++)
        n += fb_text_w(w->items[i]) + 16 + (cb ? cb + 4 : 0);
    return n + ((w->flags & UI_TF_PLUS) ? 16 : 0);
}

/* the widest a dropdown's longest item needs (it shows one at a time, and the
 * arrow box eats the right end of the field) */
static int items_needed_w(const unoui_widget *w)
{
    int i, n = 0;
    if (!w->items) return 0;
    for (i = 0; i < w->nitems; i++)
        { int t = fb_text_w(w->items[i]); if (t > n) n = t; }
    return n;
}

int unoui_window_audit(const unoui_theme *t, const unoui_window *win,
                       unoui_audit_fn cb, void *ctx)
{
    int i, n = 0, cw, ch;
    if (!t || !win || !cb) return 0;
    /* the content box render_window clips widgets to */
    if (win->flags & UI_WIN_BARE) { cw = win->r.w; ch = win->r.h; }
    else {
        cw = win->r.w - 2 * (t->m.frame_w + t->m.pad);
        ch = win->r.h - t->m.title_h - t->m.pad - t->m.frame_w;
    }
    /* A window that SCROLLS is meant to be taller than its frame - that is what
     * the scrollbar is for - so the bottom edge is not a limit for it. The
     * width still is: nothing scrolls sideways, and the bar takes a strip off
     * the right that widgets must not be laid out under. */
    if (win->flags & UI_WIN_VSCROLL) {
        if (win->content_h > ch) ch = win->content_h;
        cw -= UI_WIN_BAR_W;
    }
    for (i = 0; i < win->nw; i++) {
        const unoui_widget *w = &win->w[i];
        if (w->kind == UI_MENUBAR) continue;          /* spans the frame by design */
        if (w->r.x < 0 || w->r.y < 0)
            { cb(ctx, win, i, "starts outside the content area", w->r, cw, ch); n++; continue; }
        if (w->r.x + w->r.w > cw)
            { cb(ctx, win, i, "runs past the right edge", w->r, cw, ch); n++; continue; }
        if (w->r.y + w->r.h > ch)
            { cb(ctx, win, i, "runs past the bottom edge", w->r, cw, ch); n++; continue; }
        if (text_overflows(w))
            { cb(ctx, win, i, "text is wider than its control", w->r, cw, ch); n++; continue; }
        if (w->kind == UI_TABS && tabs_needed_w(w) > w->r.w)
            { cb(ctx, win, i, "tab strip is wider than its rect", w->r, cw, ch); n++; continue; }
        if (w->kind == UI_DROPDOWN && items_needed_w(w) + 24 > w->r.w)
            { cb(ctx, win, i, "an item is wider than the dropdown", w->r, cw, ch); n++; continue; }
    }
    return n;
}

int unoui_ui_audit(const unoui_ui *ui, unoui_audit_fn cb, void *ctx)
{
    int i, n = 0;
    if (!ui) return 0;
    for (i = 0; i < ui->nwin; i++) n += unoui_window_audit(ui->theme, ui->win[i], cb, ctx);
    return n;
}

/* ---- scrolling lists ------------------------------------------------------
 * A list box shows a WINDOW of its items: `top` is the first visible row. All
 * of the geometry lives here so the painter, the hit test and the input layer
 * agree by construction, and so a CANVAS app (which has no widgets, only a
 * rect) can host the same scrolling list - see unoui.h.  */

int unoui_list_rows(unoui_rect r)
{
    int rows = (r.h - 4) / ui_row_h();
    return rows < 1 ? 1 : rows;
}

int unoui_list_maxtop(unoui_rect r, int n)
{
    int mt = n - unoui_list_rows(r);
    return mt > 0 ? mt : 0;
}

int unoui_list_index_at(unoui_rect r, int n, int top, int y)
{
    int i = (y - (r.y + 3)) / ui_row_h();
    if (i < 0) i = 0;
    if (i > unoui_list_rows(r) - 1) i = unoui_list_rows(r) - 1;
    i += top;
    if (i > n - 1) i = n - 1;
    if (i < 0) i = 0;
    return i;
}

int unoui_list_reveal(unoui_rect r, int n, int sel, int top)
{
    int rows = unoui_list_rows(r), mt = unoui_list_maxtop(r, n);
    if (sel >= 0 && sel < n) {
        if (sel < top) top = sel;
        else if (sel > top + rows - 1) top = sel - rows + 1;
    }
    if (top > mt) top = mt;
    if (top < 0) top = 0;
    return top;
}

void unoui_list_set_sel(unoui_widget *w, int sel)
{
    if (!w) return;
    w->sel = sel;
    w->flags |= UI_WF_LIST_REVEAL;      /* the next draw scrolls it into view */
}

/* the scrollbar strip, or a zero-width rect when the list does not overflow */
unoui_rect unoui_list_bar(unoui_rect r, int n)
{
    unoui_rect bar = { r.x + r.w - UI_LIST_BAR_W, r.y, UI_LIST_BAR_W, r.h };
    if (unoui_list_maxtop(r, n) <= 0 || r.w <= 2 * UI_LIST_BAR_W) bar.w = 0;
    return bar;
}

void unoui_list_draw(const unoui_theme *t, unoui_rect r, const char **items,
                     int n, int sel, int top)
{
    const unoui_draw *d = t->draw ? t->draw : &unoui_default_draw;
    unoui_rect bar = unoui_list_bar(r, n), body = r;
    int mt = unoui_list_maxtop(r, n);
    if (top > mt) top = mt;
    if (top < 0) top = 0;
    if (bar.w) body.w -= bar.w;
    PICK(list)(t, body, items, n, sel, top);
    if (bar.w) {
        /* A scrollbar painter sizes its thumb as track/(track+vmax), i.e. it
         * reads vmax in the SAME UNITS as the track's pixel height. A list's
         * scroll range is in ROWS (a dozen or so), which next to a track of a
         * hundred-odd pixels makes the thumb fill almost the whole bar however
         * long the list is (metal: "the draggable part fills the entire
         * height"). Convert the row range into that pixel domain, so the thumb
         * ends up the honest rows/n fraction of the track. */
        int rows = unoui_list_rows(r);
        int track = bar.h - 2 * UI_LIST_BAR_W;
        int vpx, mpx;
        if (track < 8) track = 8;
        mpx = track * mt / rows;                    /* thumb = rows/n of track */
        vpx = mt > 0 ? mpx * top / mt : 0;          /* same fraction travelled */
        PICK(vscroll)(t, bar, vpx, mpx);
    }
}

/* ---- tabbed documents (UI_TABS) -----------------------------------------
 * ONE layout pass feeds the painter, every public rect and the hit test, so
 * they cannot drift apart - see unoui.h. It is pure arithmetic on the strip
 * rect plus fb_text_w(); nothing here keeps state. The cost is that a rect
 * query re-runs the pass, which for a handful of tabs is a few string widths
 * and buys the guarantee that a click can only land where something was drawn.
 *
 * A zero-flag model is a plain strip and is handed straight to the theme, so
 * the Control Panel and any theme that overrides `tabs` are untouched.  */

typedef struct {
    unoui_rect area;          /* the span tabs are laid out in                */
    unoui_rect plus, over;    /* trailing controls; .w 0 = absent             */
    int tab_w;                /* elastic width, or 0 = size each to its text  */
    int cb;                   /* close-box side, 0 = no close boxes           */
    int ctrl;                 /* trailing control side                        */
    int first, maxfirst, nvis, used;
} tabs_lay;

int unoui_tabs_h(const unoui_theme *t) { (void)t; return UI_TAB_H; }

static unoui_rect zero_rect(void) { unoui_rect q; q.x = q.y = q.w = q.h = 0; return q; }

static int tab_cell_w(const unoui_tabs_model *m, int i, int cb, int tab_w)
{
    if (tab_w) return tab_w;
    return fb_text_w(m->labels[i]) + 16 + (cb ? cb + 4 : 0);
}

static int elastic_w(int avail, int n)
{
    int tw = n > 0 ? avail / n : 0;
    if (tw > UI_TAB_MAX_W) tw = UI_TAB_MAX_W;
    if (tw < UI_TAB_MIN_W) tw = UI_TAB_MIN_W;
    return tw;
}

static tabs_lay lay_tabs(const unoui_theme *t, unoui_rect r, const unoui_tabs_model *m)
{
    tabs_lay L;
    int i, avail, need = 0;
    (void)t;
    L.area = r; L.plus = L.over = zero_rect();
    L.tab_w = L.cb = L.ctrl = 0;
    L.first = L.maxfirst = L.nvis = L.used = 0;
    if (!m || !m->labels || m->n <= 0 || r.w <= 0 || r.h <= 0) { L.area.w = 0; return L; }

    L.ctrl = r.h - 2;
    if (L.ctrl < 10) L.ctrl = 10;
    if (L.ctrl > r.w) L.ctrl = r.w;
    if (m->flags & UI_TF_CLOSE) {
        L.cb = r.h - 10;
        if (L.cb < 7)  L.cb = 7;
        if (L.cb > 12) L.cb = 12;
    }

    avail = r.w - ((m->flags & UI_TF_PLUS) ? L.ctrl : 0);
    if (avail < 0) avail = 0;

    /* Does the whole set fit without an overflow control? Answered BEFORE the
     * `first` clamp, because the answer must not depend on where the strip is
     * scrolled to - otherwise the ">>" would appear and vanish as you scroll. */
    if (m->flags & UI_TF_ELASTIC) need = elastic_w(avail, m->n) * m->n;
    else for (i = 0; i < m->n; i++) need += tab_cell_w(m, i, L.cb, 0);

    if (need > avail && (m->flags & UI_TF_OVERFLOW)) {
        avail -= L.ctrl;
        if (avail < 0) avail = 0;
        L.over.x = r.x + r.w - L.ctrl; L.over.y = r.y + 1;
        L.over.w = L.ctrl;             L.over.h = r.h - 2;
    }
    if (m->flags & UI_TF_ELASTIC) L.tab_w = elastic_w(avail, m->n);
    L.area.w = avail;

    /* largest legal first = the smallest start whose tail still fits */
    if (m->flags & UI_TF_OVERFLOW) {
        int acc = 0, k = m->n;
        while (k > 0 && acc + tab_cell_w(m, k - 1, L.cb, L.tab_w) <= avail)
            acc += tab_cell_w(m, --k, L.cb, L.tab_w);
        L.maxfirst = k > m->n - 1 ? m->n - 1 : k;
        if (L.maxfirst < 0) L.maxfirst = 0;
    }
    L.first = m->first;
    if (L.first > L.maxfirst) L.first = L.maxfirst;
    if (L.first < 0) L.first = 0;

    for (i = L.first; i < m->n; i++) {
        int wq = tab_cell_w(m, i, L.cb, L.tab_w);
        if (L.nvis && L.used + wq > avail) break;   /* the first always draws */
        L.used += wq; L.nvis++;
    }
    if (L.used > avail) L.used = avail;

    if (m->flags & UI_TF_PLUS) {
        int px = L.area.x + L.used;                 /* follows the last tab   */
        if (px > L.area.x + L.area.w) px = L.area.x + L.area.w;
        L.plus.x = px; L.plus.y = r.y + 1; L.plus.w = L.ctrl; L.plus.h = r.h - 2;
    }
    return L;
}

void unoui_tabs_model_of(const unoui_widget *w, unoui_tabs_model *m)
{
    if (!w || !m) return;
    m->labels   = (const char *const *)w->items;
    m->n        = w->nitems;
    m->sel      = w->sel;
    m->hot      = -1;
    m->hot_part = UI_TAB_NONE;
    m->first    = w->value;
    m->flags    = w->flags & UI_TF_ANY;
}

int unoui_tabs_visible(const unoui_theme *t, unoui_rect r, const unoui_tabs_model *m)
{ return lay_tabs(t, r, m).nvis; }

int unoui_tabs_maxfirst(const unoui_theme *t, unoui_rect r, const unoui_tabs_model *m)
{ return lay_tabs(t, r, m).maxfirst; }

int unoui_tabs_reveal(const unoui_theme *t, unoui_rect r,
                      const unoui_tabs_model *m, int sel)
{
    tabs_lay L = lay_tabs(t, r, m);
    int first = L.first;
    if (m && sel >= 0 && sel < m->n) {
        if (sel < first) first = sel;
        else if (sel >= first + L.nvis) {
            /* walk the start forward until `sel` is the last tab that fits */
            unoui_tabs_model probe = *m;
            int f = sel;
            while (f > 0) {
                tabs_lay L2;
                probe.first = f - 1;
                L2 = lay_tabs(t, r, &probe);
                if (L2.first + L2.nvis <= sel) break;
                f--;
            }
            first = f;
        }
    }
    if (first > L.maxfirst) first = L.maxfirst;
    if (first < 0) first = 0;
    return first;
}

unoui_rect unoui_tab_rect(const unoui_theme *t, unoui_rect r,
                          const unoui_tabs_model *m, int i)
{
    tabs_lay L = lay_tabs(t, r, m);
    unoui_rect q = zero_rect();
    int k, x;
    if (!m || i < L.first || i >= L.first + L.nvis) return q;
    x = L.area.x;
    for (k = L.first; k < i; k++) x += tab_cell_w(m, k, L.cb, L.tab_w);
    q.x = x; q.y = r.y; q.h = r.h;
    q.w = tab_cell_w(m, i, L.cb, L.tab_w);
    if (q.x + q.w > L.area.x + L.area.w) q.w = L.area.x + L.area.w - q.x;
    if (q.w < 0) q.w = 0;
    return q;
}

unoui_rect unoui_tab_close_rect(const unoui_theme *t, unoui_rect r,
                                const unoui_tabs_model *m, int i)
{
    tabs_lay L = lay_tabs(t, r, m);
    unoui_rect tr = unoui_tab_rect(t, r, m, i), q = zero_rect();
    if (!L.cb || tr.w <= 0) return q;
    q.w = q.h = L.cb;
    q.x = tr.x + tr.w - L.cb - 5;
    q.y = tr.y + (tr.h - L.cb) / 2;
    /* a tab clipped narrow by the strip edge carries no close box, so a click
     * there selects rather than closing - never the destructive reading */
    if (q.x < tr.x + 4) return zero_rect();
    return q;
}

unoui_rect unoui_tabs_plus_rect(const unoui_theme *t, unoui_rect r,
                                const unoui_tabs_model *m)
{ return lay_tabs(t, r, m).plus; }

unoui_rect unoui_tabs_over_rect(const unoui_theme *t, unoui_rect r,
                                const unoui_tabs_model *m)
{ return lay_tabs(t, r, m).over; }

/* copy as much of `s` as fits in `maxw` pixels into `buf` */
static void fit_label(const char *s, int maxw, char *buf, int cap)
{
    int k = 0;
    buf[0] = 0;
    if (maxw <= 0) return;
    while (s[k] && k < cap - 1) {
        buf[k] = s[k]; buf[k + 1] = 0;
        if (fb_text_w(buf) > maxw) { buf[k] = 0; return; }
        k++;
    }
}

static void draw_ctrl_box(const unoui_theme *t, unoui_rect b, int hot)
{
    fb_fill_rect(b.x, b.y, b.w, b.h, hot ? t->pal.light : t->pal.face);
    fb_frame_rect(b.x, b.y, b.w, b.h, t->pal.shadow);
}

void unoui_tabs_draw(const unoui_theme *t, unoui_rect r, const unoui_tabs_model *m)
{
    const unoui_draw *d = t->draw ? t->draw : &unoui_default_draw;
    tabs_lay L;
    int i;
    if (!m || !m->labels || m->n <= 0) return;
    if (!(m->flags & UI_TF_ANY)) {          /* a plain strip belongs to the theme */
        PICK(tabs)(t, r, (const char **)m->labels, m->n, m->sel, 0);
        return;
    }
    L = lay_tabs(t, r, m);
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.face);
    fb_hline(r.x, r.y + r.h - 1, r.w, t->pal.dark);

    for (i = L.first; i < L.first + L.nvis; i++) {
        unoui_rect q = unoui_tab_rect(t, r, m, i), cbr;
        int on = (i == m->sel), hot = (m->hot == i);
        int top = on ? q.y : q.y + 2;
        int h   = r.y + r.h - top - (on ? 0 : 1);
        char lbl[48];
        if (q.w <= 0) continue;
        fb_fill_rect(q.x, top, q.w, h,
                     on ? t->pal.win_bg : (hot ? t->pal.light : t->pal.face));
        fb_hline(q.x, top, q.w, t->pal.dark);
        fb_vline(q.x, top, h, t->pal.light);
        fb_vline(q.x + q.w - 1, top, h, t->pal.shadow);
        if (on) {                            /* the active underline */
            fb_hline(q.x, r.y + r.h - 2, q.w, t->pal.accent);
            fb_hline(q.x, r.y + r.h - 1, q.w, t->pal.accent);
        }
        fit_label(m->labels[i], q.w - 12 - (L.cb ? L.cb + 4 : 0), lbl, (int)sizeof lbl);
        fb_text(q.x + 6, top + (h - fb_text_h()) / 2, lbl,
                on ? t->pal.text : t->pal.text_dim, -1);

        cbr = unoui_tab_close_rect(t, r, m, i);
        if (cbr.w > 0) {
            fb_px c = (hot && m->hot_part == UI_TAB_CLOSE)
                      ? t->pal.accent : t->pal.text_dim;
            int half = cbr.w / 2, k;
            for (k = -half + 1; k <= half - 1; k++) {
                fb_pixel(cbr.x + half + k, cbr.y + half + k, c);
                fb_pixel(cbr.x + half + k, cbr.y + half - k, c);
            }
        }
    }

    if (L.plus.w > 0) {
        unoui_rect b = L.plus;
        draw_ctrl_box(t, b, m->hot_part == UI_TAB_PLUS);
        fb_hline(b.x + 4, b.y + b.h / 2, b.w - 8, t->pal.text);
        fb_vline(b.x + b.w / 2, b.y + 4, b.h - 8, t->pal.text);
    }
    if (L.over.w > 0) {
        unoui_rect b = L.over;
        int tw = fb_text_w(">>");
        draw_ctrl_box(t, b, m->hot_part == UI_TAB_OVER);
        fb_text(b.x + (b.w - tw) / 2, b.y + (b.h - fb_text_h()) / 2, ">>",
                t->pal.text, -1);
    }
}

int unoui_tabs_hit(const unoui_theme *t, unoui_rect r, const unoui_tabs_model *m,
                   int x, int y, int *which)
{
    tabs_lay L;
    int i;
    if (which) *which = -1;
    if (!m || !m->labels || m->n <= 0) return UI_TAB_NONE;
    if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) return UI_TAB_NONE;
    L = lay_tabs(t, r, m);
    if (L.over.w > 0 && x >= L.over.x && x < L.over.x + L.over.w) return UI_TAB_OVER;
    if (L.plus.w > 0 && x >= L.plus.x && x < L.plus.x + L.plus.w) return UI_TAB_PLUS;
    for (i = L.first; i < L.first + L.nvis; i++) {
        unoui_rect q = unoui_tab_rect(t, r, m, i), c;
        if (q.w <= 0 || x < q.x || x >= q.x + q.w) continue;
        if (which) *which = i;
        c = unoui_tab_close_rect(t, r, m, i);
        if (c.w > 0 && x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h)
            return UI_TAB_CLOSE;
        return UI_TAB_SEL;
    }
    return UI_TAB_NONE;
}

/* ---- MDI: child frames inside a widget -----------------------------------
 * See unoui.h for what a child is and, more importantly, is not. Everything
 * here is arithmetic on the container rect plus the theme metrics; the only
 * state is the app's own unoui_mdi.
 *
 * z[] and focus hold index + 1 with 0 meaning "none", so a zero-initialised
 * unoui_mdi reads as empty. Storing a bare index terminated by -1 is what cost
 * WM phase E a mid-gate reboot: 0 is a valid index and bss reads as all-zero. */

static void draw_resize_grip(const unoui_theme *t, const unoui_window *win); /* fwd */

static int mdi_min_w(const unoui_mdi *m) { return m->min_w > 0 ? m->min_w : UI_MDI_MIN_W; }
static int mdi_min_h(const unoui_mdi *m) { return m->min_h > 0 ? m->min_h : UI_MDI_MIN_H; }

static int mdi_live(const unoui_mdi *m, int i)
{ return m && m->ch && i >= 0 && i < m->cap && m->ch[i].used; }

int unoui_mdi_focused(const unoui_mdi *m)
{ return (m && m->focus) ? (int)m->focus - 1 : -1; }

int unoui_mdi_count(const unoui_mdi *m)
{
    int k = 0;
    if (!m) return 0;
    while (k < UNOUI_MDI_MAX && m->z[k]) k++;
    return k;
}

int unoui_mdi_zorder(const unoui_mdi *m, int k)
{
    if (!m || k < 0 || k >= UNOUI_MDI_MAX || !m->z[k]) return -1;
    return (int)m->z[k] - 1;
}

/* position of child `i` in the z-list, or -1 */
static int mdi_zindex(const unoui_mdi *m, int i)
{
    int k, n = unoui_mdi_count(m);
    for (k = 0; k < n; k++) if (m->z[k] == (unsigned char)(i + 1)) return k;
    return -1;
}

int unoui_mdi_add(unoui_mdi *m, const char *title, int x, int y, int w, int h,
                  int flags, unoui_canvas *c)
{
    int i, n;
    if (!m || !m->ch) return -1;
    n = unoui_mdi_count(m);
    if (n >= UNOUI_MDI_MAX) return -1;
    for (i = 0; i < m->cap && m->ch[i].used; i++) ;
    if (i >= m->cap) return -1;
    m->ch[i].r.x = x; m->ch[i].r.y = y; m->ch[i].r.w = w; m->ch[i].r.h = h;
    m->ch[i].title = title; m->ch[i].flags = flags; m->ch[i].canvas = c;
    m->ch[i].used = 1;
    m->z[n] = (unsigned char)(i + 1);
    if (n + 1 < UNOUI_MDI_MAX) m->z[n + 1] = 0;
    m->focus = (unsigned char)(i + 1);
    return i;
}

void unoui_mdi_close(unoui_mdi *m, int i)
{
    int k, n;
    if (!mdi_live(m, i)) return;
    m->ch[i].used = 0;
    n = unoui_mdi_count(m);
    k = mdi_zindex(m, i);
    if (k >= 0) {
        for (; k < n - 1; k++) m->z[k] = m->z[k + 1];
        m->z[n - 1] = 0;
    }
    n = unoui_mdi_count(m);            /* focus falls to whatever is now front */
    m->focus = n ? m->z[n - 1] : 0;
}

void unoui_mdi_raise(unoui_mdi *m, int i)
{
    int k, n;
    if (!mdi_live(m, i)) return;
    n = unoui_mdi_count(m);
    k = mdi_zindex(m, i);
    if (k >= 0) {
        for (; k < n - 1; k++) m->z[k] = m->z[k + 1];
        m->z[n - 1] = (unsigned char)(i + 1);
    }
    m->focus = (unsigned char)(i + 1);
}

unoui_rect unoui_mdi_child_rect(unoui_rect r, const unoui_mdi *m, int i)
{
    unoui_rect q = zero_rect();
    if (!mdi_live(m, i)) return q;
    q = m->ch[i].r;
    q.x += r.x; q.y += r.y;
    return q;
}

unoui_rect unoui_mdi_content_rect(const unoui_theme *t, unoui_rect r,
                                  const unoui_mdi *m, int i)
{
    unoui_rect q = unoui_mdi_child_rect(r, m, i);
    int fw, th;
    if (q.w <= 0 || !t) return zero_rect();
    fw = t->m.frame_w; th = t->m.title_h;
    q.x += fw; q.y += th; q.w -= 2 * fw; q.h -= th + fw;
    if (q.w < 0) q.w = 0;
    if (q.h < 0) q.h = 0;
    return q;
}

void unoui_mdi_clamp(unoui_rect r, unoui_mdi *m, int i)
{
    unoui_mdi_child *c;
    if (!mdi_live(m, i)) return;
    c = &m->ch[i];
    if (c->r.w > r.w) c->r.w = r.w;
    if (c->r.h > r.h) c->r.h = r.h;
    if (c->r.w < mdi_min_w(m)) c->r.w = mdi_min_w(m);
    if (c->r.h < mdi_min_h(m)) c->r.h = mdi_min_h(m);
    if (c->r.x + c->r.w > r.w) c->r.x = r.w - c->r.w;
    if (c->r.y + c->r.h > r.h) c->r.y = r.h - c->r.h;
    if (c->r.x < 0) c->r.x = 0;
    if (c->r.y < 0) c->r.y = 0;
}

int unoui_mdi_at(unoui_rect r, const unoui_mdi *m, int x, int y)
{
    int k;
    for (k = unoui_mdi_count(m) - 1; k >= 0; k--) {   /* front to back */
        int i = unoui_mdi_zorder(m, k);
        unoui_rect q = unoui_mdi_child_rect(r, m, i);
        if (q.w > 0 && x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + q.h)
            return i;
    }
    return -1;
}

void unoui_mdi_tile(const unoui_theme *t, unoui_rect r, unoui_mdi *m)
{
    int n = unoui_mdi_count(m), cols = 1, rows, k;
    (void)t;
    if (n <= 0) return;
    while (cols * cols < n) cols++;                   /* ceil(sqrt(n)) */
    rows = (n + cols - 1) / cols;
    for (k = 0; k < n; k++) {
        int i = unoui_mdi_zorder(m, k), cx = k % cols, cy = k / cols;
        unoui_mdi_child *c = &m->ch[i];
        /* edges from the same a*i/n form on both sides, so adjacent cells
         * share a boundary exactly and integer division leaves no seam */
        c->r.x = r.w * cx / cols;
        c->r.y = r.h * cy / rows;
        c->r.w = r.w * (cx + 1) / cols - c->r.x;
        c->r.h = r.h * (cy + 1) / rows - c->r.y;
        unoui_mdi_clamp(r, m, i);
    }
}

void unoui_mdi_cascade(const unoui_theme *t, unoui_rect r, unoui_mdi *m)
{
    int n = unoui_mdi_count(m), k;
    int step = t ? t->m.title_h : 18;
    int w = r.w * 3 / 4, h = r.h * 3 / 4;
    if (n <= 0) return;
    if (step < 8) step = 8;
    /* shrink the step so the whole stack still fits rather than letting the
     * clamp pile the tail on top of each other in the bottom-right corner */
    if (n > 1) {
        int sx = (r.w > w) ? (r.w - w) / (n - 1) : 0;
        int sy = (r.h > h) ? (r.h - h) / (n - 1) : 0;
        if (step > sx) step = sx;
        if (step > sy) step = sy;
        if (step < 2) step = 2;
    }
    for (k = 0; k < n; k++) {
        int i = unoui_mdi_zorder(m, k);
        unoui_mdi_child *c = &m->ch[i];
        c->r.x = k * step; c->r.y = k * step;
        c->r.w = w;        c->r.h = h;
        unoui_mdi_clamp(r, m, i);
    }
}

/* A child frame IS a window as far as the theme painters are concerned - they
 * read ->r, ->title, ->active and ->flags and write ->content_* - so handing
 * them a temporary one buys every theme's chrome with no new artwork.
 *
 * ONE reused scratch rather than a stack temporary: unoui_window embeds its
 * 64-widget array, so a local would be several KB of stack per child per frame.
 * nw = 0 means the painters never touch that array, and unoui is
 * single-threaded by construction. */
static unoui_window g_mdi_tmp;

static unoui_window *mdi_as_window(unoui_rect r, const unoui_mdi *m, int i)
{
    unoui_window *w = &g_mdi_tmp;
    const unoui_mdi_child *c = &m->ch[i];
    w->title     = c->title;
    w->r         = unoui_mdi_child_rect(r, m, i);
    w->active    = (unoui_mdi_focused(m) == i);
    /* NOCTL: minimize and maximize are meaningless without a taskbar and a
     * work area, and a drawn control that does nothing is worse than none */
    w->flags     = UI_WIN_NOCTL | ((c->flags & UI_MDI_RESIZE) ? UI_WIN_RESIZE : 0);
    w->nw        = 0;
    w->content_x = w->content_y = 0;
    w->font_slot = UI_FONT_INHERIT;
    w->min_w     = mdi_min_w(m);
    w->min_h     = mdi_min_h(m);
    w->snap      = UI_SNAP_NONE;
    w->restore_r = w->r;
    return w;
}

void unoui_mdi_draw(const unoui_theme *t, unoui_rect r, const unoui_mdi *m)
{
    const unoui_draw *d = t->draw ? t->draw : &unoui_default_draw;
    int k, n;
    if (!m || !m->ch || r.w <= 0 || r.h <= 0) return;
    n = unoui_mdi_count(m);
    fb_set_clip(r.x, r.y, r.w, r.h);        /* children never escape the box */
    for (k = 0; k < n; k++) {               /* back to front */
        int i = unoui_mdi_zorder(m, k);
        unoui_window *tw = mdi_as_window(r, m, i);
        PICK(window)(t, tw);
        PICK(titlebar)(t, tw);
        draw_resize_grip(t, tw);
        if (m->ch[i].canvas && m->ch[i].canvas->draw) {
            unoui_rect in = unoui_mdi_content_rect(t, r, m, i);
            if (in.w > 0 && in.h > 0) {
                fb_set_clip(in.x, in.y, in.w, in.h);
                /* a child has no widget of its own - the canvas identifies
                 * itself by its ctx, which is what every consumer uses */
                m->ch[i].canvas->draw(0, in, m->ch[i].canvas->ctx);
                fb_set_clip(r.x, r.y, r.w, r.h);
            }
        }
    }
    fb_reset_clip();
}

static void draw_one(const unoui_draw *d, const unoui_theme *t,
                     const unoui_window *win, unoui_widget *w, int eff, int menuopen)
{
    unoui_rect r = unoui_widget_rect(t, win, w);
    switch (w->kind) {
    case UI_LABEL:    PICK(label)(t, r, w->text, eff); break;
    case UI_BUTTON:   PICK(button)(t, r, w->text, eff); break;
    case UI_CHECK:    PICK(check)(t, r, w->text, eff); break;
    case UI_RADIO:    PICK(radio)(t, r, w->text, eff); break;
    case UI_FIELD:
        /* Reveal is TEMPORARY, and this is the one place every path that can
         * take the focus away passes through - a click elsewhere, Tab, a window
         * raise, the app rebuilding its widgets. Clearing it here rather than in
         * each of those is what makes "you cannot walk away from a screen with
         * a password on it" true rather than mostly true. */
        if (w->edit && w->edit->secret && !(eff & UI_F_FOCUS)) w->edit->revealed = 0;
        PICK(field)(t, r, w->text, w->edit, eff); break;
    case UI_TEXTAREA: PICK(textarea)(t, r, w->edit, eff); break;
    case UI_PROGRESS: PICK(progress)(t, r, w->value, w->vmax); break;
    case UI_BUSY:     PICK(busy)(t, r, w->value % UI_BUSY_DOTS); break;
    case UI_VSCROLL:  PICK(vscroll)(t, r, w->value, w->vmax); break;
    case UI_HSCROLL:  PICK(hscroll)(t, r, w->value, w->vmax); break;
    case UI_SLIDER:   PICK(slider)(t, r, w->value, w->vmin, w->vmax, eff); break;
    case UI_SPINNER:  PICK(spinner)(t, r, w->value, eff); break;
    case UI_DROPDOWN: PICK(dropdown)(t, r,
                          (w->sel >= 0 && w->sel < w->nitems) ? w->items[w->sel] : "",
                          eff); break;
    case UI_TABS: {   /* `value` is the first visible tab, as a list's is its top */
        unoui_tabs_model tm;
        unoui_tabs_model_of(w, &tm);
        w->value = unoui_tabs_reveal(t, r, &tm, -1);
        tm.first = w->value;
        unoui_tabs_draw(t, r, &tm);
        break;
    }
    case UI_MENUBAR:  PICK(menubar)(t, r, w->menus, w->nmenus, menuopen, -1); break;
    case UI_LIST:     w->value = unoui_list_reveal(r, w->nitems,
                          (w->flags & UI_WF_LIST_REVEAL) ? w->sel : -1, w->value);
                      w->flags &= ~UI_WF_LIST_REVEAL;
                      unoui_list_draw(t, r, w->items, w->nitems, w->sel, w->value);
                      break;
    case UI_GROUP:    PICK(group)(t, r, w->text); break;
    case UI_SEP:      PICK(sep)(t, r); break;
    case UI_ICON:     if (unoui_icon_art) unoui_icon_art(w->icon, r, w->text, eff);
                      else PICK(icon)(t, r, w->text, eff); break;
    case UI_CANVAS:
        /* the app draws arbitrary pixels - confine it to the canvas rect, then
         * restore the window content clip for the widgets that follow. */
        if (w->canvas && w->canvas->draw) {
            int fw = t->m.frame_w, th = t->m.title_h;
            fb_set_clip(r.x, r.y, r.w, r.h);
            w->canvas->draw(w, r, w->canvas->ctx);
            fb_set_clip(win->r.x + fw, win->r.y + th,
                        win->r.w - 2 * fw, win->r.h - th - fw);
        }
        break;
    case UI_MDI:
        /* child frames set their own clip; restore this window's afterwards for
         * whatever widgets follow, the same way UI_CANVAS does. A BARE window's
         * widgets fill its whole rect, so its clip is not the content formula. */
        if (w->mdi) {
            int fw = t->m.frame_w, th = t->m.title_h;
            unoui_mdi_draw(t, r, w->mdi);
            if (win->flags & UI_WIN_BARE)
                fb_set_clip(win->r.x, win->r.y, win->r.w, win->r.h);
            else
                fb_set_clip(win->r.x + fw, win->r.y + th,
                            win->r.w - 2 * fw, win->r.h - th - fw);
        }
        break;
    }
}

/* first UI_CANVAS widget in a window (used by fullscreen), or NULL */
static unoui_widget *first_canvas(unoui_window *win)
{
    int i;
    for (i = 0; i < win->nw; i++) if (win->w[i].kind == UI_CANVAS) return &win->w[i];
    return 0;
}

/* Paint the whole-screen backdrop: the shell's wallpaper hook first (if it
 * claims the frame), else the active theme's desktop painter. Single seam so
 * every desktop-paint path (cached, live, fullscreen restore) agrees. */
static void paint_backdrop(const unoui_theme *t, const unoui_draw *d, int W, int H)
{
    if (unoui_wallpaper && unoui_wallpaper(t, W, H)) return;
    PICK(desktop)(t, W, H);
}

void unoui_desktop(const unoui_theme *t, int W, int H)
{
    const unoui_draw *d = t->draw ? t->draw : &unoui_default_draw;
    paint_backdrop(t, d, W, H);
}

/* ---- calendar (the reusable core of a date-picker) ----------------------- *
 * Draw a month grid + hit-test it. A port builds a date picker by putting a
 * canvas over these: unoui_calendar_draw paints, unoui_calendar_hit maps a
 * click to a day (1..31) or a nav arrow (UI_CAL_PREV / UI_CAL_NEXT). */
int unoui_days_in_month(int y, int m)
{
    static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return (m >= 1 && m <= 12) ? d[m-1] : 30;
}
int unoui_day_of_week(int y, int m, int d)         /* 0 = Sunday (Sakamoto) */
{
    static const int t[12] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
    if (m < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}
static const char *kMon[12] = { "January","February","March","April","May","June",
    "July","August","September","October","November","December" };
#define CAL_HDR 20
#define CAL_WK  14

void unoui_calendar_draw(const unoui_theme *t, unoui_rect r, int y, int m, int sel)
{
    static const char *wk[7] = { "S","M","T","W","T","F","S" };
    int cw = r.w / 7, ch = (r.h - CAL_HDR - CAL_WK) / 6;
    int fdow = unoui_day_of_week(y, m, 1), dim = unoui_days_in_month(y, m), d, i;
    char hdr[32]; int hn = 0; const char *mn = kMon[((m-1)%12+12)%12];
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.win_bg);
    /* header: Month Year */
    { const char *p = mn; while (*p && hn < 20) hdr[hn++] = *p++; hdr[hn++] = ' ';
      { int yy = y, div = 1000, started = 0; if (yy < 0) yy = 0;
        while (div) { int dg = (yy/div)%10; if (dg || started || div==1) { hdr[hn++]=(char)('0'+dg); started=1; } div/=10; } }
      hdr[hn] = 0; }
    fb_text(r.x + (r.w - fb_text_w(hdr))/2, r.y + (CAL_HDR - fb_text_h())/2, hdr, t->pal.text, -1);
    { int mid = r.y + CAL_HDR/2, k;                              /* < and > chevrons */
      for (k = 0; k < 4; k++) {
          fb_vline(r.x + 6 + k,        mid - k, 1, t->pal.accent);   /* < top */
          fb_vline(r.x + 6 + k,        mid + k, 1, t->pal.accent);   /* < bottom */
          fb_vline(r.x + r.w - 7 - k,  mid - k, 1, t->pal.accent);   /* > top */
          fb_vline(r.x + r.w - 7 - k,  mid + k, 1, t->pal.accent);   /* > bottom */
      } }
    /* weekday row */
    for (i = 0; i < 7; i++)
        fb_text(r.x + i*cw + (cw - fb_text_w(wk[i]))/2, r.y + CAL_HDR + 3,
                wk[i], t->pal.text_dim, -1);
    /* day cells */
    for (d = 1; d <= dim; d++) {
        int idx = fdow + d - 1, col = idx % 7, row = idx / 7;
        int cx = r.x + col*cw, cy = r.y + CAL_HDR + CAL_WK + row*ch;
        char nb[3]; int n = 0; if (d >= 10) nb[n++] = (char)('0'+d/10); nb[n++] = (char)('0'+d%10); nb[n]=0;
        if (d == sel) fb_fill_rect(cx+2, cy, cw-4, ch-1, t->pal.accent);
        fb_text(cx + (cw - fb_text_w(nb))/2, cy + (ch - fb_text_h())/2,
                nb, d == sel ? t->pal.accent_text : t->pal.text, -1);
    }
}

int unoui_calendar_hit(unoui_rect r, int y, int m, int px, int py)
{
    int cw = r.w / 7, ch = (r.h - CAL_HDR - CAL_WK) / 6;
    if (px < r.x || px >= r.x + r.w) return UI_CAL_NONE;
    if (py >= r.y && py < r.y + CAL_HDR) {                       /* header arrows */
        if (px < r.x + cw)          return UI_CAL_PREV;
        if (px >= r.x + r.w - cw)   return UI_CAL_NEXT;
        return UI_CAL_NONE;
    }
    if (py >= r.y + CAL_HDR + CAL_WK) {                          /* day cells */
        int col = (px - r.x) / cw, row = (py - (r.y + CAL_HDR + CAL_WK)) / ch;
        int fdow = unoui_day_of_week(y, m, 1), dim = unoui_days_in_month(y, m);
        int d = row*7 + col - fdow + 1;
        if (d >= 1 && d <= dim) return d;
    }
    return UI_CAL_NONE;
}

/* ---- title-bar window controls (the generic painter) ---------------------- *
 * Themes opt in per button through unoui_metrics.minbox / .maxbox; the boxes
 * themselves are drawn from the palette by ONE painter here, exactly like the
 * resize grip, so no theme carries its own artwork. Layout: right-aligned
 * [min][max], 4 px gaps, vertically centred in the title bar the same way the
 * close box is. The hit-test (unoui_input.c) reads the same geometry, so a
 * click always lands on the pixels that were drawn. */

/* The close box, for ANY titled rect. Deliberately not part of
 * unoui_titlebtn_rect: that one refuses UI_WIN_NOCTL windows, and an MDI child
 * is exactly a NOCTL window that still has a close box. Both title-bar painters
 * and both hit tests (window and MDI child) read this, so the box can only be
 * clicked where it was drawn - the same rule the min/max boxes follow. */
unoui_rect unoui_closebox_rect(const unoui_theme *t, unoui_rect winr)
{
    unoui_rect r; int fw, th, cs;
    r.x = r.y = r.w = r.h = 0;
    if (!t || (cs = t->m.closebox) <= 0) return r;
    fw = t->m.frame_w; th = t->m.title_h;
    r.y = winr.y + fw + (th - fw - cs) / 2;
    r.w = r.h = cs;
    r.x = t->m.closeright ? winr.x + winr.w - fw - 4 - cs : winr.x + fw + 4;
    return r;
}

unoui_rect unoui_titlebtn_rect(const unoui_theme *t, const unoui_window *win, int which)
{
    unoui_rect r; int fw, th, sz, right;
    r.x = r.y = r.w = r.h = 0;
    if (!t || !win || (win->flags & (UI_WIN_BARE | UI_WIN_NOCTL))) return r;
    fw = t->m.frame_w; th = t->m.title_h;
    sz = (which == UI_TB_MIN) ? t->m.minbox : t->m.maxbox;
    if (sz <= 0) return r;                    /* theme has no such button     */
    right = win->r.x + win->r.w - fw - 4;
    /* With the close box on the right it is the OUTBOARD control - closest to
     * the corner, the way every desktop that puts it there does - so min and
     * max shuffle inboard by its width. */
    if (t->m.closeright && t->m.closebox > 0) right -= t->m.closebox + 4;
    if (which == UI_TB_MIN && t->m.maxbox > 0) right -= t->m.maxbox + 4;
    r.x = right - sz;
    r.y = win->r.y + fw + (th - fw - sz) / 2;
    r.w = r.h = sz;
    return r;
}

/* one box: face fill, the theme's edge treatment, then a palette glyph */
static void draw_title_btn(const unoui_theme *t, unoui_rect b, int which,
                           int restore, int dis)
{
    fb_px g = dis ? t->pal.text_dim : t->pal.face_text;
    unoui_rect in;
    if (b.w <= 0) return;
    fb_fill_rect(b.x, b.y, b.w, b.h, t->pal.face);
    if (t->m.bevel > 0) in = ui_bevel(b, t, t->m.bevel, 1);
    else {                                    /* flat themes get a hairline   */
        fb_frame_rect(b.x, b.y, b.w, b.h, t->pal.shadow);
        in.x = b.x + 1; in.y = b.y + 1; in.w = b.w - 2; in.h = b.h - 2;
    }
    if (in.w < 4 || in.h < 4) return;         /* too small to glyph honestly  */
    if (which == UI_TB_MIN) {                 /* a bar along the bottom       */
        fb_fill_rect(in.x + 1, in.y + in.h - 3, in.w - 2, 2, g);
    } else if (!restore) {                    /* an empty frame = maximize    */
        fb_frame_rect(in.x + 1, in.y + 1, in.w - 2, in.h - 2, g);
        fb_hline(in.x + 1, in.y + 2, in.w - 2, g);     /* its own title bar   */
    } else {                                  /* two frames = restore down    */
        fb_frame_rect(in.x + 3, in.y, in.w - 3, in.h - 3, g);
        fb_fill_rect (in.x, in.y + 3, in.w - 3, in.h - 3, t->pal.face);
        fb_frame_rect(in.x, in.y + 3, in.w - 3, in.h - 3, g);
        fb_hline     (in.x, in.y + 4, in.w - 3, g);
    }
}

int (*unoui_win_badge)(const unoui_window *);

/* Badge hue `idx`, derived from the palette's accent by rotating its channels.
 * Deriving beats naming a role: a theme guarantees exactly ONE accent, so a
 * fixed set of roles could not keep four badges apart, and hard-coded RGB would
 * ignore the theme entirely. Rotation keeps the accent's saturation and
 * brightness - so the badges stay a family - while separating their hues. */
static fb_px badge_color(const unoui_theme *t, int idx)
{
    fb_px a = t->pal.accent;
    unsigned r = a & 0xFFu, g = (a >> 8) & 0xFFu, b = (a >> 16) & 0xFFu;
    switch (idx & (UI_BADGE_N - 1)) {
    case 0:  return a;
    case 1:  return FB_RGB(b, r, g);
    case 2:  return FB_RGB(g, b, r);
    default: return FB_RGB(255u - r, 255u - g, 255u - b);
    }
}

/* The badge sits inboard of the min/max boxes (this toolkit's close box is at
 * the LEFT end of the bar, so the right end is where the free space is). A
 * theme with no title buttons at all anchors it to the same right margin they
 * would have used. .w == 0 = it does not fit / there is nothing to draw. */
#define UI_BADGE_SZ 6
static unoui_rect badge_rect(const unoui_theme *t, const unoui_window *win)
{
    unoui_rect r, b;
    int fw = t->m.frame_w, th = t->m.title_h;
    r.x = r.y = r.w = r.h = 0;
    if (win->flags & UI_WIN_BARE) return r;
    if (th - fw < UI_BADGE_SZ + 2) return r;      /* title bar too short      */
    b = unoui_titlebtn_rect(t, win, UI_TB_MIN);
    if (b.w <= 0) b = unoui_titlebtn_rect(t, win, UI_TB_MAX);
    r.x = (b.w > 0 ? b.x : win->r.x + win->r.w - fw - 4) - 4 - UI_BADGE_SZ;
    r.y = win->r.y + fw + (th - fw - UI_BADGE_SZ) / 2;
    if (r.x <= win->r.x + fw + 2) return r;       /* window too narrow: skip  */
    r.w = r.h = UI_BADGE_SZ;
    return r;
}

/* Draw whatever window controls this theme + window call for. Called from the
 * shared per-window render path (and the static contact-sheet path) right
 * after the title bar, so it paints over the theme's own chrome. */
static void draw_title_buttons(const unoui_theme *t, const unoui_window *win)
{
    int nomax;
    if (win->flags & UI_WIN_BARE) return;
    if (unoui_win_badge) {                        /* the optional group marker */
        int bi = unoui_win_badge(win);
        if (bi >= 0) {
            unoui_rect br = badge_rect(t, win);
            if (br.w > 0) {
                fb_fill_rect(br.x, br.y, br.w, br.h, badge_color(t, bi));
                fb_frame_rect(br.x, br.y, br.w, br.h, t->pal.dark);
            }
        }
    }
    /* a window that cannot resize cannot maximize: the box is drawn disabled
     * (and the shell ignores the action it emits) rather than hidden, so the
     * title bars of fixed-layout apps still line up with everybody else's. */
    nomax = !(win->flags & UI_WIN_RESIZE);
    draw_title_btn(t, unoui_titlebtn_rect(t, win, UI_TB_MIN), UI_TB_MIN, 0, 0);
    draw_title_btn(t, unoui_titlebtn_rect(t, win, UI_TB_MAX), UI_TB_MAX,
                   win->snap == UI_SNAP_MAX, nomax);
}

void unoui_render(unoui_window *win, const unoui_theme *t)
{
    const unoui_draw *d = t->draw ? t->draw : &unoui_default_draw;
    int fw = t->m.frame_w, th = t->m.title_h, i;
    PICK(window)(t, win);
    PICK(titlebar)(t, win);
    draw_title_buttons(t, win);
    fb_set_clip(win->r.x + fw, win->r.y + th, win->r.w - 2 * fw, win->r.h - th - fw);
    for (i = 0; i < win->nw; i++)
        draw_one(d, t, win, &win->w[i], win->w[i].flags, -1);
    fb_reset_clip();
}

void unoui_fullscreen(unoui_ui *ui, unoui_window *win) { ui->full = win; }

/* ---- cached desktop background (pc64) ------------------------------------- *
 * The desktop painter fills the whole screen every frame with a gradient plus
 * two large alpha-blended "aurora" blobs - static content that was being
 * recomputed on every repaint (and every drag frame). Cache it once and blit
 * it thereafter; invalidate on theme / resolution change. Gated on UNO_BG_CACHE,
 * an opt-in per-port capability (define it in the port's build when it can spare
 * an fb-sized g_bg[]), so ports that can't afford the buffer don't pay for it. */
#ifdef UNO_BG_CACHE
static fb_px g_bg[FB_BUF_PIX];
static int   g_bg_valid;
void unoui_bg_invalidate(void) { g_bg_valid = 0; }
static void draw_desktop_cached(const unoui_theme *t, const unoui_draw *d, unoui_ui *ui)
{
    size_t px = (size_t)FB_W * (size_t)FB_H;
    if (g_bg_valid) { memcpy(fb, g_bg, px * sizeof(fb_px)); return; }
    paint_backdrop(t, d, ui->screen_w, ui->screen_h);
    memcpy(g_bg, fb, px * sizeof(fb_px));
    g_bg_valid = 1;
}
#else
void unoui_bg_invalidate(void) { }
#endif

void (*unoui_profile_win)(const char *title, int begin);

/* corner grip: three diagonal ridges (light over shadow), the classic "this
 * corner drags" affordance - clearer than the old faint dot cluster the user
 * couldn't find. */
static void draw_resize_grip(const unoui_theme *t, const unoui_window *win)
{
    int fw = t->m.frame_w;
    int bx = win->r.x + win->r.w - fw, by = win->r.y + win->r.h - fw, k;
    if (!(win->flags & UI_WIN_RESIZE) || (win->flags & UI_WIN_BARE)) return;
    for (k = 0; k < 3; k++) {
        int o = 4 + k * 4, j;
        for (j = 0; j < o; j++) {
            fb_pixel(bx - o + j - 1, by - 1 - j, t->pal.light);
            fb_pixel(bx - o + j,     by - 1 - j, t->pal.shadow);
        }
    }
}

/* Everything of a window that is NOT its widgets: shadow, frame, title bar,
 * title-bar buttons, resize grip. Honours the caller's clip, so a platform
 * holding a cached image of the window's opaque interior can repaint just the
 * translucent perimeter - the only part that a MOVE invalidates, because it is
 * composited against whatever is behind it. */
static void render_window_chrome(unoui_ui *ui, unoui_window *win, int wn)
{
    const unoui_theme *t = ui->theme;
    const unoui_draw *d = t->draw ? t->draw : &unoui_default_draw;
    if (win->flags & UI_WIN_BARE) return;   /* desktop/taskbar have no chrome */
    win->active = (wn == ui->focus_win);
    PICK(window)(t, win);
    PICK(titlebar)(t, win);
    draw_title_buttons(t, win);
    draw_resize_grip(t, win);
}

void unoui_render_window_chrome(unoui_ui *ui, unoui_window *win)
{
    int wn;
    if (!win) return;
    for (wn = 0; wn < ui->nwin; wn++) if (ui->win[wn] == win) break;
    render_window_chrome(ui, win, wn < ui->nwin ? wn : -1);
}

/* One window, chrome + widgets, with the interaction state its z-index `wn`
 * implies (focus, hot, pressed, the open menu). Shared by unoui_render_ui and
 * unoui_render_window so a platform repainting a single window during a live
 * drag gets pixels identical to a full scene pass. */
static void render_one_window(unoui_ui *ui, unoui_window *win, int wn)
{
    const unoui_theme *t = ui->theme;
    const unoui_draw *d = t->draw ? t->draw : &unoui_default_draw;
    int fw = t->m.frame_w, th = t->m.title_h, i;

    win->active = (wn == ui->focus_win);
    if (unoui_profile_win)
        unoui_profile_win((win->flags & UI_WIN_BARE) ? "(shell)" :
                          (win->title ? win->title : "(untitled)"), 1);
    if (win->flags & UI_WIN_BARE) {
        /* shell chrome (desktop / taskbar): no frame or titlebar; widgets
         * fill the whole window rect and are clipped to it. */
        fb_set_clip(win->r.x, win->r.y, win->r.w, win->r.h);
    } else {
        /* chrome (frame, titlebar, shadow) draws unclipped - its painters
         * are authored not to overflow win->r. */
        render_window_chrome(ui, win, wn);
        /* widgets are confined to the content rect (the region d_window
         * fills below the titlebar) so an over-sized widget or a too-long
         * label is cut at the frame instead of spilling onto the desktop. */
        fb_set_clip(win->r.x + fw, win->r.y + th,
                    win->r.w - 2 * fw, win->r.h - th - fw);
    }
    { int fontpushed = (win->font_slot != UI_FONT_INHERIT && unoui_font_push);
      if (fontpushed) unoui_font_push(win->font_slot);    /* per-window doc font */
    for (i = 0; i < win->nw; i++) {
        unoui_widget *w = &win->w[i];
        int eff = w->flags, menuopen = -1;
        if (wn == ui->focus_win && i == ui->focus_wi) {
            eff |= UI_F_FOCUS;
            if (w->edit && ((ui->ticks / CARET_BLINK) & 1u) == 0) eff |= UI_F_CARET;
        }
        if (ui->cap_mode == UI_CAP_BUTTON && wn == ui->cap_win && i == ui->cap_wi)
            eff |= UI_F_PRESSED;
        if (wn == ui->hot_win && i == ui->hot_wi) eff |= UI_F_HOT;
        if (w->kind == UI_MENUBAR && ui->popup_wi == i && ui->popup_win == wn)
            menuopen = ui->popup_menu;
        draw_one(d, t, win, w, eff, menuopen);
    }
      if (fontpushed) unoui_font_pop(); }
    fb_reset_clip();
    /* The content scrollbar, painted AFTER the widgets and outside their clip:
     * it is chrome, not content, and a bar that scrolled with the thing it
     * scrolls would be a joke. */
    { unoui_rect bar = unoui_win_bar(t, win);
      if (bar.w) {
          int mx = unoui_win_scroll_max(t, win);
          PICK(vscroll)(t, bar, win->scroll_y, mx);
      } }
    draw_resize_grip(t, win);
    if (unoui_profile_win)
        unoui_profile_win((win->flags & UI_WIN_BARE) ? "(shell)" :
                          (win->title ? win->title : "(untitled)"), 0);
}

void unoui_render_window(unoui_ui *ui, unoui_window *win)
{
    int wn;
    if (!win) return;
    for (wn = 0; wn < ui->nwin; wn++) if (ui->win[wn] == win) break;
    render_one_window(ui, win, wn < ui->nwin ? wn : -1);
}

void unoui_render_ui(unoui_ui *ui)
{
    const unoui_theme *t = ui->theme;
    const unoui_draw *d = t->draw ? t->draw : &unoui_default_draw;
    int wn;

    /* Let a geometry animation settle this frame's rects BEFORE anything is
     * drawn from them. Here rather than in the platform's frame loop because a
     * window whose size is mid-flight also needs its fill widgets reflowed, and
     * a port that forgot the call would get a window frame that moves with the
     * content standing still. Nothing to do when no animator is installed. */
    if (unoui_geom_tick) unoui_geom_tick(ui);

    /* fullscreen: the window's canvas owns the whole screen, no chrome. Clear
     * first - an app canvas (a game) may not paint every pixel, and without the
     * clear the desktop/windows from the prior frame would show through. */
    if (ui->full) {
        unoui_widget *cv = first_canvas(ui->full);
        if (unoui_profile_win)
            unoui_profile_win(ui->full->title ? ui->full->title : "(fullscreen)", 1);
        fb_fill_rect(0, 0, ui->screen_w, ui->screen_h, FB_RGB(0, 0, 0));
        if (cv && cv->canvas && cv->canvas->draw) {
            unoui_rect fs = { 0, 0, ui->screen_w, ui->screen_h };
            cv->canvas->draw(cv, fs, cv->canvas->ctx);
        }
        if (unoui_profile_win)
            unoui_profile_win(ui->full->title ? ui->full->title : "(fullscreen)", 0);
        return;
    }

#ifdef UNO_BG_CACHE
    draw_desktop_cached(t, d, ui);
#else
    paint_backdrop(t, d, ui->screen_w, ui->screen_h);
#endif
    for (wn = 0; wn < ui->nwin; wn++)
        render_one_window(ui, ui->win[wn], wn);
    /* the open menu's popup deliberately extends past its window - draw it
     * after the clip is reset. */
    if (ui->popup_wi >= 0)
        PICK(popup)(t, ui->popup_r, ui->popup_items, ui->popup_n, ui->popup_hot);

    /* rubber-band drag outline (the window itself hasn't moved yet) */
    unoui_draw_drag_outline(ui);
    /* drag-to-edge snap target. Drawn last, over everything: a full-scene pass
     * mid-drag is the rare path (pc64 uses the snapshot fast path, which paints
     * the preview UNDER the dragged window), and a preview hidden behind a
     * window would be worse than one washing over it. */
    unoui_draw_snap_preview(ui);
}

/* Draw just the rubber-band outline. Split out so a platform can render the
 * scene once, snapshot it, and per drag frame restore + redraw only this
 * outline instead of repainting the whole scene. No-op when not dragging. */
void unoui_draw_drag_outline(unoui_ui *ui)
{
    const unoui_theme *t = ui->theme;
    if (ui->drag_active) {
        int x = ui->drag_x, y = ui->drag_y, w = ui->drag_w, h = ui->drag_h;
        fb_frame_rect(x,     y,     w,     h,     t->pal.dark);
        fb_frame_rect(x + 1, y + 1, w - 2, h - 2, t->pal.accent);
        fb_frame_rect(x + 2, y + 2, w - 4, h - 4, t->pal.dark);
    }
}

/* ---- snapping ------------------------------------------------------------- *
 * Geometry only: halves and quarters of the live work area, derived at CALL
 * time, so a taskbar or font change just re-derives instead of leaving stale
 * rects behind. */

/* Drag-preview wash strength (0..255). Light enough that the desktop and any
 * window under the target stay legible, heavy enough to read as a highlight on
 * every theme's accent. */
#define SNAP_PREVIEW_ALPHA 56

unoui_rect unoui_snap_rect(const unoui_ui *ui, int snap)
{
    unoui_rect wk = unoui_work_area(ui), r = wk;
    int hw = wk.w / 2, hh = wk.h / 2;
    switch (snap) {
    case UI_SNAP_L:  r.w = hw; break;
    case UI_SNAP_R:  r.w = wk.w - hw; r.x = wk.x + hw; break;
    case UI_SNAP_TL: r.w = hw; r.h = hh; break;
    case UI_SNAP_TR: r.w = wk.w - hw; r.x = wk.x + hw; r.h = hh; break;
    case UI_SNAP_BL: r.w = hw; r.h = wk.h - hh; r.y = wk.y + hh; break;
    case UI_SNAP_BR: r.w = wk.w - hw; r.x = wk.x + hw;
                     r.h = wk.h - hh; r.y = wk.y + hh; break;
    default: break;                          /* UI_SNAP_MAX / NONE: the lot   */
    }
    return r;
}

/* The rect snapping `win` to `snap` will ACTUALLY produce: the snap rect for a
 * resizable window, and the centred move-only rect for one with a fixed layout.
 * unoui_snap_apply and the drag preview both go through here, so what the
 * preview paints and what the release commits cannot drift apart. */
static unoui_rect snap_target(const unoui_ui *ui, const unoui_window *win, int snap)
{
    unoui_rect tr = unoui_snap_rect(ui, snap);
    if (win && !(win->flags & UI_WIN_RESIZE)) {
        unoui_rect r = win->r;
        r.x = tr.x + (tr.w - r.w) / 2;
        r.y = tr.y + (tr.h - r.h) / 2;
        if (r.x < tr.x) r.x = tr.x;
        if (r.y < tr.y) r.y = tr.y;
        return r;
    }
    return tr;
}

void unoui_draw_snap_preview(unoui_ui *ui)
{
    const unoui_theme *t = ui->theme;
    const unoui_window *win = 0;
    unoui_rect r;
    if (!ui->snap_preview) return;
    if (ui->cap_mode == UI_CAP_WINDOW && ui->cap_win >= 0 && ui->cap_win < ui->nwin)
        win = ui->win[ui->cap_win];
    r = snap_target(ui, win, ui->snap_preview);
    /* palette only: a wash light enough to read the desktop through, plus a
     * hard edge so the target reads on a flat two-tone theme (Win 3.1) where
     * the wash alone is a subtle tint. */
    fb_blend_rect(r.x, r.y, r.w, r.h, t->pal.accent, SNAP_PREVIEW_ALPHA);
    fb_frame_rect(r.x, r.y, r.w, r.h, t->pal.accent);
}

unoui_geom_fn      unoui_geom_anim = 0;
unoui_geom_tick_fn unoui_geom_tick = 0;
int unoui_snap_ms = 130;      /* long enough to read, short enough to feel instant */

/* Put `win` at `r`, animated if an animator is installed and takes the job.
 * Every geometry change in unoui_snap_apply goes through here, so the six
 * routes into a snap - drag-to-edge, the maximize box, double-click, the
 * context menu, the keyboard, tile/cascade - all behave the same way without
 * any of them knowing an animation exists. */
static void geom_to(unoui_ui *ui, unoui_window *win, unoui_rect r)
{
    if (unoui_geom_anim && unoui_snap_ms > 0 &&
        unoui_geom_anim(ui, win, r, unoui_snap_ms)) return;
    win->r = r;
}

void unoui_snap_apply(unoui_ui *ui, unoui_window *win, int snap)
{
    unoui_rect tr;
    if (!win) return;
    if (snap == UI_SNAP_NONE) {
        /* only a window that IS snapped has a rect to give back; restore_r is
         * zero on one that never snapped, and putting that back would collapse
         * it to nothing. */
        if (win->snap != UI_SNAP_NONE && win->restore_r.w > 0)
            geom_to(ui, win, win->restore_r);
        win->snap = UI_SNAP_NONE;
        unoui_reflow_window(ui->theme, win);
        return;
    }
    tr = snap_target(ui, win, snap);
    if (!(win->flags & UI_WIN_RESIZE)) {
        /* A fixed pixel layout must not be stretched, so a non-resizable window
         * is only MOVED into the target and keeps snap NONE - it has no snap
         * state to leave, and nothing to restore. snap_target already centred
         * it, so the rect below IS the move. */
        geom_to(ui, win, tr);
        return;
    }
    /* entering a snap from free-floating remembers the rect ONCE: re-snapping
     * between states must still restore the original, not the previous snap. */
    if (win->snap == UI_SNAP_NONE) win->restore_r = win->r;
    geom_to(ui, win, tr);
    win->snap = (unsigned char)snap;
    unoui_reflow_window(ui->theme, win);
}

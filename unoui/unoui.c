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

/* ------------------------------------------------------------------ build -- */

static unoui_widget *push(unoui_window *win, ui_kind k, int x, int y,
                          int w, int h, const char *text)
{
    unoui_widget *wd;
    if (win->nw >= UNOUI_MAX_WIDGETS) return &win->w[UNOUI_MAX_WIDGETS - 1];
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

/* ---- editable text model ------------------------------------------------- */
void unoui_text_init(unoui_text *t, char *buf, int cap, int multiline)
{
    int n = 0; while (buf[n] && n < cap - 1) n++;
    t->buf = buf; t->cap = cap; t->len = n;
    t->caret = n; t->sel = n; t->scroll_x = t->scroll_y = 0;
    t->multiline = multiline;
}

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
        int cs = t->m.closebox, cy = bar.y + (bar.h - cs) / 2;
        unoui_rect cb = { bar.x + 4, cy, cs, cs };
        ui_bevel(cb, t, 1, 1);
        bar.x += cs + 8; bar.w -= cs + 8;
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
    *cx = in.x + 3 + ui_seg_w(t->buf, s, s + col) - t->scroll_x;
    *cy = t->multiline ? in.y + 2 + line * UI_LINE_H - t->scroll_y
                       : in.y + (in.h - fb_text_h()) / 2;
}

int ui_text_index_at(unoui_rect in, const unoui_text *t, int px, int py)
{
    int line = 0, s, e, i, x, want;
    if (t->multiline) {
        line = (py - (in.y + 2) + t->scroll_y) / UI_LINE_H;
        if (line < 0) line = 0;
        if (line > text_lines(t) - 1) line = text_lines(t) - 1;
    }
    line_span(t, line, &s, &e);
    /* walk the line's glyphs; snap to whichever gap is nearest the pointer */
    want = px - (in.x + 3) + t->scroll_x;
    x = 0;
    for (i = s; i < e; i++) {
        int cw = ui_seg_w(t->buf, i, i + 1);
        if (want < x + cw / 2) return i;
        x += cw;
    }
    return e;
}

void ui_text_reveal(unoui_rect in, unoui_text *t)
{
    int line, col, s, e, cpx, vis_w = in.w - 6, vis_h = in.h - 4;
    idx_linecol(t, t->caret, &line, &col);
    line_span(t, line, &s, &e);
    cpx = 3 + ui_seg_w(t->buf, s, s + col);
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

static void draw_edit_text(unoui_rect in, const unoui_text *t,
                           const unoui_theme *th, int focused, int caret_on)
{
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
                int sx = x0 + ui_seg_w(t->buf, s, a);
                clamp_fill(in, sx, y - 1, ui_seg_w(t->buf, a, b), lh, th->pal.accent);
            }
        }
        /* draw up to three runs so selection recolours without breaking the
         * glyph flow: [s,a) normal, [a,b) selected, [b,e) normal */
        { int a = focused && selB > selA ? (selA > s ? (selA < e ? selA : e) : s) : e;
          int b = focused && selB > selA ? (selB > s ? (selB < e ? selB : e) : s) : e;
          int x = x0;
          x = seg_draw(t->buf, s, a, x, y, th->pal.field_text, -1);
          x = seg_draw(t->buf, a, b, x, y, th->pal.accent_text, -1);
          (void)seg_draw(t->buf, b, e, x, y, th->pal.field_text, -1); }
    }
    if (focused && caret_on) {
        int cx, cy; ui_text_caret_xy(in, t, t->caret, &cx, &cy);
        if (cx >= in.x && cx <= in.x + in.w)
            fb_vline(cx, cy - 1, fh + 1, th->pal.field_text);
    }
    fb_set_clip(scx, scy, scw, sch);
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

static void d_list(const unoui_theme *t, unoui_rect r, const char **it, int n, int sel)
{
    int i, row = ui_row_h(), y;
    unoui_rect in;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.field_bg);
    in = ui_bevel(r, t, 1, -1);
    for (i = 0, y = in.y + 2; i < n && y + row <= in.y + in.h; i++, y += row) {
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
    d_menubar, d_popup
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
      { unoui_rect r = { ox + w->r.x, oy + w->r.y, w->r.w, w->r.h }; return r; } }
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
    case UI_FIELD:    PICK(field)(t, r, w->text, w->edit, eff); break;
    case UI_TEXTAREA: PICK(textarea)(t, r, w->edit, eff); break;
    case UI_PROGRESS: PICK(progress)(t, r, w->value, w->vmax); break;
    case UI_VSCROLL:  PICK(vscroll)(t, r, w->value, w->vmax); break;
    case UI_HSCROLL:  PICK(hscroll)(t, r, w->value, w->vmax); break;
    case UI_SLIDER:   PICK(slider)(t, r, w->value, w->vmin, w->vmax, eff); break;
    case UI_SPINNER:  PICK(spinner)(t, r, w->value, eff); break;
    case UI_DROPDOWN: PICK(dropdown)(t, r,
                          (w->sel >= 0 && w->sel < w->nitems) ? w->items[w->sel] : "",
                          eff); break;
    case UI_TABS:     PICK(tabs)(t, r, w->items, w->nitems, w->sel, eff); break;
    case UI_MENUBAR:  PICK(menubar)(t, r, w->menus, w->nmenus, menuopen, -1); break;
    case UI_LIST:     PICK(list)(t, r, w->items, w->nitems, w->sel); break;
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
    }
}

/* first UI_CANVAS widget in a window (used by fullscreen), or NULL */
static unoui_widget *first_canvas(unoui_window *win)
{
    int i;
    for (i = 0; i < win->nw; i++) if (win->w[i].kind == UI_CANVAS) return &win->w[i];
    return 0;
}

void unoui_desktop(const unoui_theme *t, int W, int H)
{
    const unoui_draw *d = t->draw ? t->draw : &unoui_default_draw;
    PICK(desktop)(t, W, H);
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

void unoui_render(unoui_window *win, const unoui_theme *t)
{
    const unoui_draw *d = t->draw ? t->draw : &unoui_default_draw;
    int fw = t->m.frame_w, th = t->m.title_h, i;
    PICK(window)(t, win);
    PICK(titlebar)(t, win);
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
    PICK(desktop)(t, ui->screen_w, ui->screen_h);
    memcpy(g_bg, fb, px * sizeof(fb_px));
    g_bg_valid = 1;
}
#else
void unoui_bg_invalidate(void) { }
#endif

void unoui_render_ui(unoui_ui *ui)
{
    const unoui_theme *t = ui->theme;
    const unoui_draw *d = t->draw ? t->draw : &unoui_default_draw;
    int wn, i;

    /* fullscreen: the window's canvas owns the whole screen, no chrome. Clear
     * first - an app canvas (a game) may not paint every pixel, and without the
     * clear the desktop/windows from the prior frame would show through. */
    if (ui->full) {
        unoui_widget *cv = first_canvas(ui->full);
        fb_fill_rect(0, 0, ui->screen_w, ui->screen_h, FB_RGB(0, 0, 0));
        if (cv && cv->canvas && cv->canvas->draw) {
            unoui_rect fs = { 0, 0, ui->screen_w, ui->screen_h };
            cv->canvas->draw(cv, fs, cv->canvas->ctx);
        }
        return;
    }

#ifdef UNO_BG_CACHE
    draw_desktop_cached(t, d, ui);
#else
    PICK(desktop)(t, ui->screen_w, ui->screen_h);
#endif
    for (wn = 0; wn < ui->nwin; wn++) {
        unoui_window *win = ui->win[wn];
        int fw = t->m.frame_w, th = t->m.title_h;
        win->active = (wn == ui->focus_win);
        if (win->flags & UI_WIN_BARE) {
            /* shell chrome (desktop / taskbar): no frame or titlebar; widgets
             * fill the whole window rect and are clipped to it. */
            fb_set_clip(win->r.x, win->r.y, win->r.w, win->r.h);
        } else {
            /* chrome (frame, titlebar, shadow) draws unclipped - its painters
             * are authored not to overflow win->r. */
            PICK(window)(t, win);
            PICK(titlebar)(t, win);
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
        if ((win->flags & UI_WIN_RESIZE) && !(win->flags & UI_WIN_BARE)) {
            /* corner grip: three diagonal ridges (light over shadow), the
             * classic "this corner drags" affordance - clearer than the old
             * faint dot cluster the user couldn't find. */
            int bx = win->r.x + win->r.w - fw, by = win->r.y + win->r.h - fw, k;
            for (k = 0; k < 3; k++) {
                int o = 4 + k * 4, j;
                for (j = 0; j < o; j++) {
                    fb_pixel(bx - o + j - 1, by - 1 - j, t->pal.light);
                    fb_pixel(bx - o + j,     by - 1 - j, t->pal.shadow);
                }
            }
        }
    }
    /* the open menu's popup deliberately extends past its window - draw it
     * after the clip is reset. */
    if (ui->popup_wi >= 0)
        PICK(popup)(t, ui->popup_r, ui->popup_items, ui->popup_n, ui->popup_hot);

    /* rubber-band drag outline (the window itself hasn't moved yet) */
    unoui_draw_drag_outline(ui);
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

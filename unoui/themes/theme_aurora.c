/* theme_aurora - a modern "unique synthesis" look for unoui, drawing from
 * Windows 11 (Mica/rounded), macOS (clean surfaces, left close, translucency)
 * and Material (tonal surfaces, one strong accent). GRAPHICS theming: a custom
 * painter vtable built on the fb alpha-blend / gradient primitives, so it gets
 * soft drop shadows, anti-aliased rounded corners, flat tonal surfaces and an
 * accent signature (an underline beneath the active title bar). Ships in two
 * palettes - Aurora Light (default) and Aurora Dark - over the one vtable.
 *
 * Everything here is additive: other themes/ports are untouched (they never set
 * radius and use the default painters). */
#include "../unoui_theme.h"
#include "fb.h"

/* ------------------------------------------------------------------ helpers */
static int clampi(int v, int lo, int hi){ return v<lo?lo:v>hi?hi:v; }

/* rounded fills come from fb (fb_round_rect_a); these are thin aliases so the
 * painters read cleanly. Corner bits mirror FB_CORNER_*. */
enum { C_TL=FB_CORNER_TL, C_TR=FB_CORNER_TR, C_BL=FB_CORNER_BL, C_BR=FB_CORNER_BR,
       C_ALL=FB_CORNER_ALL, C_TOP=FB_CORNER_TOP };
static void aa_fill_a(int x,int y,int w,int h,int rad,fb_px c,int alpha,int corners)
{ fb_round_rect_a(x, y, w, h, rad, c, alpha, corners); }
static void aa_fill(int x,int y,int w,int h,int rad,fb_px c)
{ fb_round_rect_a(x, y, w, h, rad, c, 255, FB_CORNER_ALL); }

/* a rounded 1px-ish border: fill the border colour, then inset the interior. */
static void aa_border(int x,int y,int w,int h,int rad,int t,fb_px border,fb_px inner)
{
    aa_fill_a(x, y, w, h, rad, border, 255, C_ALL);
    aa_fill_a(x+t, y+t, w-2*t, h-2*t, rad-t>0?rad-t:0, inner, 255, C_ALL);
}

/* Aurora compositing level (see unoui_theme.h). 0 = full alpha, 1 = lite/static. */
int unoui_aurora_lite = 0;

/* soft drop shadow: several expanding low-alpha rounded layers, offset down.
 * In LITE mode there is no per-frame alpha: the window simply sits flat (a 1px
 * frame gives it definition), so the shadow is skipped entirely. */
static void soft_shadow(int x,int y,int w,int h,int rad)
{
    int i;
    if (unoui_aurora_lite) return;
    for (i = 6; i >= 1; i--)
        aa_fill_a(x-i, y-i+4, w+2*i, h+2*i, rad+i, FB_RGB(0,0,0), 8, C_ALL);
}

/* window/title corner radius: 0 in LITE (squared corners avoid the anti-aliased
 * per-pixel alpha on the curves), the theme radius in FULL. */
static int a_rad(const unoui_theme *t) { return unoui_aurora_lite ? 0 : t->m.radius; }

/* ---------------------------------------------------------------- painters */
static void a_desktop(const unoui_theme *t, int W, int H)
{
    int gw = W*3/5, gh = H*3/5;
    fb_grad_v(0, 0, W, H, t->pal.desktop, t->pal.desktop2);
    /* two faint aurora blobs in the accent hue for depth */
    aa_fill_a(W-gw-30, -gh/3, gw, gh, gh/2, t->pal.accent, 12, C_ALL);
    aa_fill_a(-gw/4, H-gh*2/3, gw, gh, gh/2, t->pal.accent, 7, C_ALL);
}

static void a_window(const unoui_theme *t, unoui_window *win)
{
    unoui_rect r = win->r;
    int fw = t->m.frame_w, rad = a_rad(t);
    soft_shadow(r.x, r.y, r.w, r.h, rad);
    aa_border(r.x, r.y, r.w, r.h, rad, fw > 0 ? fw : 1, t->pal.win_frame, t->pal.win_bg);
    unoui_content_origin(t, win, &win->content_x, &win->content_y);
}

static void a_titlebar(const unoui_theme *t, const unoui_window *win)
{
    unoui_rect r = win->r;
    int fw = t->m.frame_w, th = t->m.title_h, rad = a_rad(t);
    int cs = t->m.closebox, cbx = r.x + fw + 6, cby = r.y + fw + (th - fw - cs)/2;
    fb_px bg = win->active ? t->pal.title_bg : t->pal.title_bg_in;
    fb_px fg = win->active ? t->pal.title_fg : t->pal.title_fg_in;
    unoui_rect bar;
    /* title strip: only the top corners rounded, meets content squarely */
    aa_fill_a(r.x+fw, r.y+fw, r.w-2*fw, th-fw, rad-fw>0?rad-fw:0, bg, 255, C_TOP);
    /* accent underline on the active window (the signature) */
    if (win->active)
        fb_fill_rect(r.x+fw+12, r.y+th-2, r.w-2*fw-24, 2, t->pal.accent);
    /* close box: a soft rounded button with an x */
    if (cs) {
        int k;
        aa_fill(cbx, cby, cs, cs, cs/3, win->active ? t->pal.face : t->pal.title_bg_in);
        for (k = 3; k < cs-3; k++) {
            fb_blend_pixel(cbx+k, cby+k, t->pal.text_dim, 220);
            fb_blend_pixel(cbx+cs-1-k, cby+k, t->pal.text_dim, 220);
        }
        bar.x = cbx + cs + 8;
    } else bar.x = r.x + fw + 10;
    bar.y = r.y + fw; bar.w = (r.x + r.w - fw) - bar.x - 6; bar.h = th - fw;
    ui_text_in(bar, win->title, fg, -1, 0);
}

static void a_button(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    int rad = 6;
    int def = (f & UI_F_DEFAULT) != 0, press = (f & UI_F_PRESSED) != 0;
    int hot = (f & UI_F_HOT) != 0, dis = (f & UI_F_DISABLED) != 0;
    fb_px face = def ? t->pal.accent : t->pal.face;
    fb_px txt  = def ? t->pal.accent_text : t->pal.face_text;
    if (def) aa_fill(r.x, r.y, r.w, r.h, rad, face);
    else     aa_border(r.x, r.y, r.w, r.h, rad, 1, t->pal.dark, face);
    if (press)     aa_fill_a(r.x, r.y, r.w, r.h, rad, FB_RGB(0,0,0), 34, C_ALL);
    else if (hot)  aa_fill_a(r.x, r.y, r.w, r.h, rad, t->pal.accent, 28, C_ALL);
    if (dis) txt = t->pal.text_dim;
    ui_text_in(r, s, txt, -1, 1);
}

static void a_check(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    int on = (f & UI_F_CHECKED) != 0, i;
    unoui_rect b = { r.x, r.y, 14, 14 };
    if (on) aa_fill(b.x, b.y, 14, 14, 4, t->pal.accent);
    else    aa_border(b.x, b.y, 14, 14, 4, 1, t->pal.dark, t->pal.field_bg);
    if (on) for (i = 0; i < 4; i++) {                 /* a small check mark */
        fb_blend_pixel(b.x+3+i,  b.y+6+i, t->pal.accent_text, 255);
        fb_blend_pixel(b.x+3+i,  b.y+8+i, t->pal.accent_text, 160);
    }
    if (on) for (i = 0; i < 6; i++) fb_blend_pixel(b.x+6+i, b.y+9-i, t->pal.accent_text, 255);
    fb_text(r.x + 20, r.y + 3, s, (f & UI_F_DISABLED) ? t->pal.text_dim : t->pal.text, -1);
}

static void a_field(const unoui_theme *t, unoui_rect r, const char *s,
                    unoui_text *ed, int f)
{
    unoui_rect in;
    aa_border(r.x, r.y, r.w, r.h, 5, 1,
              (f & UI_F_FOCUS) ? t->pal.accent : t->pal.dark, t->pal.field_bg);
    in.x = r.x + 5; in.y = r.y + 1; in.w = r.w - 10; in.h = r.h - 2;
    (void)ed;                                         /* editable text: default geometry */
    fb_text(in.x, in.y + (in.h - 8)/2, s ? s : "", t->pal.field_text, -1);
    if (f & UI_F_CARET)
        fb_vline(in.x + fb_text_w(s ? s : ""), in.y + 2, in.h - 4, t->pal.field_text);
}

static void a_list(const unoui_theme *t, unoui_rect r, const char **it, int n, int sel)
{
    int i, row = 13, y;
    aa_border(r.x, r.y, r.w, r.h, 6, 1, t->pal.win_frame, t->pal.field_bg);
    for (i = 0, y = r.y + 5; i < n && y + row <= r.y + r.h - 3; i++, y += row) {
        fb_px fg = t->pal.field_text;
        if (i == sel) { aa_fill(r.x+4, y-2, r.w-8, row, 5, t->pal.accent); fg = t->pal.accent_text; }
        fb_text(r.x + 9, y, it[i], fg, -1);
    }
}

static void a_group(const unoui_theme *t, unoui_rect r, const char *s)
{
    int rad = 8;
    aa_fill_a(r.x, r.y + 5, r.w, r.h - 5, rad, t->pal.win_frame, 255, C_ALL);
    aa_fill_a(r.x + 1, r.y + 6, r.w - 2, r.h - 7, rad-1, t->pal.win_bg, 255, C_ALL);
    if (s && *s) {
        int tw = fb_text_w(s);
        fb_fill_rect(r.x + 10, r.y, tw + 8, 9, t->pal.win_bg);
        fb_text(r.x + 14, r.y, s, t->pal.text_dim, -1);
    }
}

static void a_sep(const unoui_theme *t, unoui_rect r)
{
    fb_blend_rect(r.x, r.y, r.w, 1, t->pal.text_dim, 60);
}

static void a_progress(const unoui_theme *t, unoui_rect r, int v, int vm)
{
    int fill = vm > 0 ? clampi((r.w-4) * v / vm, 0, r.w-4) : 0;
    aa_fill(r.x, r.y, r.w, r.h, r.h/2, t->pal.field_bg);
    aa_fill_a(r.x, r.y, r.w, r.h, r.h/2, t->pal.dark, 90, C_ALL);
    if (fill > 0) aa_fill(r.x+2, r.y+2, fill, r.h-4, (r.h-4)/2, t->pal.accent);
}

static void a_vscroll(const unoui_theme *t, unoui_rect r, int v, int vm)
{
    int track_h = r.h, thumb_h, thumb_y;
    aa_fill_a(r.x + r.w/2 - 2, r.y, 4, r.h, 2, t->pal.text_dim, 40, C_ALL);  /* faint track */
    thumb_h = vm > 0 ? (track_h * track_h) / (track_h + vm) : track_h;
    if (thumb_h < 18) thumb_h = 18;
    thumb_y = r.y + (vm > 0 ? (track_h - thumb_h) * v / vm : 0);
    aa_fill(r.x + r.w/2 - 3, thumb_y, 6, thumb_h, 3, t->pal.dark);
}

static void a_dropdown(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    int i, cx = r.x + r.w - 14, cy = r.y + r.h/2 - 1;
    aa_border(r.x, r.y, r.w, r.h, 5, 1,
              (f & UI_F_FOCUS) ? t->pal.accent : t->pal.dark, t->pal.field_bg);
    fb_text(r.x + 8, r.y + (r.h - 8)/2, s ? s : "", t->pal.field_text, -1);
    for (i = 0; i < 4; i++) fb_hline(cx - i, cy - i, 2*i+1, t->pal.text_dim);  /* chevron */
}

static void a_menubar(const unoui_theme *t, unoui_rect r, const unoui_menu *m,
                      int n, int open, int hot)
{
    int i, x = r.x + 4;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.win_bg);
    fb_blend_rect(r.x, r.y + r.h - 1, r.w, 1, t->pal.text_dim, 50);
    for (i = 0; i < n; i++) {
        int tw = fb_text_w(m[i].title) + 16;
        if (i == hot || i == open) { aa_fill(x, r.y + 1, tw, r.h - 2, 4, t->pal.accent);
            fb_text(x + 8, r.y + (r.h - 8)/2, m[i].title, t->pal.accent_text, -1); }
        else fb_text(x + 8, r.y + (r.h - 8)/2, m[i].title, t->pal.text, -1);
        x += tw + 2;
    }
}

static void a_spinner(const unoui_theme *t, unoui_rect r, int v, int f)
{
    char b[16]; int i, k = 0, neg = v < 0; unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    int bw = r.h;
    unoui_rect up = { r.x + r.w - bw, r.y, bw, r.h };
    (void)f;
    aa_border(r.x, r.y, r.w - bw - 1, r.h, 5, 1, t->pal.dark, t->pal.field_bg);
    { char tmp[16]; int m=0; if(!u) tmp[m++]='0'; while(u){tmp[m++]=(char)('0'+u%10);u/=10;} if(neg)b[k++]='-'; while(m)b[k++]=tmp[--m]; b[k]=0; }
    fb_text(r.x + 6, r.y + (r.h-8)/2, b, t->pal.field_text, -1);
    aa_fill(up.x, up.y, bw, r.h, 5, t->pal.face);
    for (i = 0; i < 3; i++) { fb_hline(up.x+bw/2-i, up.y+3+i, 2*i+1, t->pal.face_text);
        fb_hline(up.x+bw/2-(2-i), up.y+r.h-4-i, 2*(2-i)+1, t->pal.face_text); }
}

static void a_slider(const unoui_theme *t, unoui_rect r, int v, int vmin, int vmax, int f)
{
    int cy = r.y + r.h/2, span = vmax - vmin, kx;
    (void)f;
    aa_fill(r.x, cy - 2, r.w, 4, 2, t->pal.field_bg);
    aa_fill_a(r.x, cy - 2, r.w, 4, 2, t->pal.dark, 80, C_ALL);
    kx = span > 0 ? r.x + (r.w - 12) * (v - vmin) / span : r.x;
    aa_fill(r.x, cy - 2, (kx - r.x) + 6, 4, 2, t->pal.accent);   /* filled track */
    aa_fill(kx, cy - 7, 12, 14, 6, t->pal.accent);              /* knob */
    aa_fill_a(kx, cy - 7, 12, 14, 6, FB_RGB(255,255,255), 40, C_ALL);
}

static const unoui_draw aurora_draw = {
    a_desktop, a_window, a_titlebar, a_button, a_check, /*radio*/0, a_field,
    /*label*/0, a_progress, a_vscroll, a_list, a_group, a_sep, /*icon*/0,
    /*textarea*/0, /*hscroll*/0, a_slider, a_spinner, a_dropdown, /*tabs*/0,
    a_menubar, /*popup*/0
};

/* -------------------------------------------------------------- palettes */
#define MET { /*title_h*/26, /*frame_w*/1, /*bevel*/0, /*pad*/12, /*radius*/9, \
              /*closebox*/13, /*shadow_off*/6, /*title_center*/0, UNOUI_DEPTH_FULL }

const unoui_theme theme_aurora_light = {
    "Aurora Light",
    {
        /* desktop  */ FB_RGB(0xE9,0xED,0xF7), FB_RGB(0xEC,0xF2,0xF2),
        /* win bg   */ FB_RGB(0xFB,0xFC,0xFE),
        /* frame    */ FB_RGB(0xD6,0xDB,0xE6),
        /* title    */ FB_RGB(0xFF,0xFF,0xFF), FB_RGB(0x1B,0x20,0x2B),
        /* title in */ FB_RGB(0xF4,0xF6,0xFA), FB_RGB(0x8C,0x93,0xA2),
        /* text     */ FB_RGB(0x22,0x27,0x31), FB_RGB(0x8C,0x93,0xA2),
        /* face     */ FB_RGB(0xEE,0xF1,0xF7), FB_RGB(0x24,0x2A,0x36),
        /* light    */ FB_RGB(0xFF,0xFF,0xFF),
        /* shadow   */ FB_RGB(0xC4,0xCB,0xD8),
        /* dark     */ FB_RGB(0xC0,0xC7,0xD5),
        /* accent   */ FB_RGB(0x4C,0x6E,0xF5), FB_RGB(0xFF,0xFF,0xFF),
        /* field    */ FB_RGB(0xFF,0xFF,0xFF), FB_RGB(0x22,0x27,0x31)
    },
    MET, &aurora_draw
};

const unoui_theme theme_aurora_dark = {
    "Aurora Dark",
    {
        /* desktop  */ FB_RGB(0x16,0x19,0x22), FB_RGB(0x10,0x14,0x1D),
        /* win bg   */ FB_RGB(0x22,0x26,0x30),
        /* frame    */ FB_RGB(0x34,0x3A,0x47),
        /* title    */ FB_RGB(0x2A,0x2F,0x3A), FB_RGB(0xEC,0xEF,0xF6),
        /* title in */ FB_RGB(0x22,0x26,0x30), FB_RGB(0x8C,0x94,0xA4),
        /* text     */ FB_RGB(0xE7,0xEB,0xF3), FB_RGB(0x8C,0x94,0xA4),
        /* face     */ FB_RGB(0x30,0x36,0x43), FB_RGB(0xE7,0xEB,0xF3),
        /* light    */ FB_RGB(0x3E,0x45,0x53),
        /* shadow   */ FB_RGB(0x0A,0x0C,0x12),
        /* dark     */ FB_RGB(0x40,0x47,0x56),
        /* accent   */ FB_RGB(0x6E,0x8B,0xFF), FB_RGB(0x0C,0x10,0x1A),
        /* field    */ FB_RGB(0x1A,0x1E,0x27), FB_RGB(0xE7,0xEB,0xF3)
    },
    MET, &aurora_draw
};

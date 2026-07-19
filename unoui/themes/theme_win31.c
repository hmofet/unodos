/* theme_win31 - Windows 3.1. GRAPHICS theming: overrides window/titlebar/
 * button/check to get the signature grey 3D chrome - blue caption bar with a
 * control-menu box + min/max buttons, and the double-bevel raised button
 * (white/black outer, ltgrey/dkgrey inner). The rest fall back to the default
 * painters, which already bevel via the light/shadow/dark roles below. */
#include "../unoui_theme.h"

#define WIN_FACE   FB_RGB(0xC0,0xC0,0xC0)
#define WIN_LIGHT  FB_RGB(0xFF,0xFF,0xFF)
#define WIN_GREY   FB_RGB(0x80,0x80,0x80)
#define WIN_DARK   FB_RGB(0x00,0x00,0x00)
#define WIN_BLUE   FB_RGB(0x00,0x00,0xA8)
#define WIN_TEAL   FB_RGB(0x00,0x80,0x80)

static void w_window(const unoui_theme *t, unoui_window *win)
{
    unoui_rect r = win->r;
    /* thin black outline, then the raised grey sizing border */
    fb_frame_rect(r.x, r.y, r.w, r.h, WIN_DARK);
    { unoui_rect b = { r.x+1, r.y+1, r.w-2, r.h-2 };
      fb_fill_rect(b.x, b.y, b.w, b.h, WIN_FACE);
      ui_bevel(b, t, 1, 1); }
    fb_fill_rect(r.x+4, r.y+t->m.title_h, r.w-8, r.h-t->m.title_h-4, t->pal.win_bg);
    unoui_content_origin(t, win, &win->content_x, &win->content_y);
}

static void caption_btn(const unoui_theme *t, int x, int y, int s, const char *glyph)
{
    unoui_rect b = { x, y, s, s };
    fb_fill_rect(b.x, b.y, s, s, WIN_FACE);
    ui_bevel(b, t, 1, 1);
    fb_text(x + (s-8)/2, y + (s-8)/2, glyph, WIN_DARK, -1);
}

static void w_titlebar(const unoui_theme *t, const unoui_window *win)
{
    unoui_rect r = win->r;
    int th = t->m.title_h;
    unoui_rect bar = { r.x+4, r.y+3, r.w-8, th-4 };
    fb_px bg = win->active ? WIN_BLUE : WIN_GREY;
    int s = th - 6, cy = r.y + 3;
    fb_fill_rect(bar.x, bar.y, bar.w, bar.h, bg);
    /* control-menu box at left (a "minus" in a box) */
    caption_btn(t, bar.x, cy, s, "-");
    /* min / max at right */
    caption_btn(t, r.x + r.w - 4 - s, cy, s, "\x18");          /* up arrow-ish */
    caption_btn(t, r.x + r.w - 4 - 2*s - 1, cy, s, "\x19");
    /* centred caption */
    { unoui_rect c = { bar.x + s + 2, bar.y, bar.w - 2*s - 6, bar.h };
      ui_text_in(c, win->title, WIN_LIGHT, -1, 1); }
}

static void w_button(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    int press = (f & UI_F_PRESSED) != 0;
    (void)t;
    if (f & UI_F_DEFAULT)                                     /* heavy black ring */
        fb_frame_rect(r.x-2, r.y-2, r.w+4, r.h+4, WIN_DARK);
    fb_fill_rect(r.x, r.y, r.w, r.h, WIN_FACE);
    if (!press) {
        fb_frame_rect(r.x, r.y, r.w, r.h, WIN_DARK);          /* outer: white/black */
        fb_hline(r.x+1, r.y+1, r.w-2, WIN_LIGHT);
        fb_vline(r.x+1, r.y+1, r.h-2, WIN_LIGHT);
        fb_hline(r.x+1, r.y+r.h-2, r.w-2, WIN_GREY);          /* inner: ltgrey/dkgrey */
        fb_vline(r.x+r.w-2, r.y+1, r.h-2, WIN_GREY);
    } else {
        fb_frame_rect(r.x, r.y, r.w, r.h, WIN_DARK);
        fb_hline(r.x+1, r.y+1, r.w-2, WIN_GREY);
        fb_vline(r.x+1, r.y+1, r.h-2, WIN_GREY);
        r.x++; r.y++;
    }
    ui_text_in(r, s, (f & UI_F_DISABLED) ? WIN_GREY : WIN_DARK, -1, 1);
}

static void w_check(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    unoui_rect box = { r.x, r.y, 13, 13 };
    (void)t;
    fb_fill_rect(box.x, box.y, 13, 13, WIN_LIGHT);
    /* sunken: black/grey TL, white BR */
    fb_hline(box.x, box.y, 13, WIN_GREY); fb_vline(box.x, box.y, 13, WIN_GREY);
    fb_hline(box.x, box.y+12, 13, WIN_LIGHT); fb_vline(box.x+12, box.y, 13, WIN_LIGHT);
    fb_frame_rect(box.x, box.y, 12, 12, WIN_DARK);
    if (f & UI_F_CHECKED) {
        int i; for (i = 0; i < 5; i++) {                      /* a check mark */
            ui_px(box.x+3, box.y+5+i, WIN_DARK);
            ui_px(box.x+4, box.y+6+i, WIN_DARK);
        }
        for (i = 0; i < 6; i++) { ui_px(box.x+5+i, box.y+8-i, WIN_DARK);
                                  ui_px(box.x+5+i, box.y+9-i, WIN_DARK); }
    }
    fb_text(r.x + 18, r.y + (r.h - fb_text_h())/2, s, (f & UI_F_DISABLED) ? WIN_GREY : WIN_DARK, -1);
}

static const unoui_draw win31_draw = {
    0, w_window, w_titlebar, w_button, w_check, 0, 0,
    0, 0, 0, 0, 0, 0, 0
};

const unoui_theme theme_win31 = {
    "Windows 3.1",
    {
        /* desktop  */ WIN_TEAL, WIN_TEAL,
        /* win bg   */ WIN_FACE,
        /* frame    */ WIN_DARK,
        /* title    */ WIN_BLUE, WIN_LIGHT,
        /* title in */ WIN_GREY, FB_RGB(0xC0,0xC0,0xC0),
        /* text     */ WIN_DARK, WIN_GREY,
        /* face     */ WIN_FACE, WIN_DARK,
        /* light    */ WIN_LIGHT, /* shadow */ WIN_GREY, /* dark */ WIN_DARK,
        /* accent   */ WIN_BLUE, WIN_LIGHT,
        /* field    */ WIN_LIGHT, WIN_DARK
    },
    { /* title_h */ 20, /* frame_w */ 4, /* bevel */ 1, /* pad */ 10,
      /* radius */ 0, /* closebox */ 0, /* shadow */ 0, /* title_center */ 1,
      UNOUI_DEPTH_4 },
    &win31_draw
};

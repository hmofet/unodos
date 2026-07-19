/* theme_macplus - Macintosh System 1-6 / Mac Plus. The 1-BIT test: depth =
 * UNOUI_DEPTH_1, so every shade in the toolkit becomes an ordered-dither
 * stipple. The desktop is the iconic 50% grey, scrollbar tracks dither to
 * grey, and there is not a single non-pure-black/white pixel. Proves the
 * write-once chrome survives a 1-bit display. Graphics: thinner racing-stripe
 * title bar, square close box, 1px drop shadow, rounded black-outline buttons. */
#include "../unoui_theme.h"

#define MP_BLACK FB_RGB(0x00,0x00,0x00)
#define MP_WHITE FB_RGB(0xFF,0xFF,0xFF)

static void p_desktop(const unoui_theme *t, int W, int H)
{
    (void)t;
    ui_stipple(0, 0, W, H, MP_WHITE, MP_BLACK, 8);   /* 50% grey */
}

static void p_window(const unoui_theme *t, unoui_window *win)
{
    unoui_rect r = win->r;
    fb_fill_rect(r.x+2, r.y+2, r.w, r.h, MP_BLACK);  /* 2px drop shadow */
    fb_fill_rect(r.x, r.y, r.w, r.h, MP_WHITE);
    fb_frame_rect(r.x, r.y, r.w, r.h, MP_BLACK);     /* double frame */
    fb_frame_rect(r.x+1, r.y+1, r.w-2, r.h-2, MP_BLACK);
    fb_hline(r.x+1, r.y + t->m.title_h, r.w-2, MP_BLACK);
    unoui_content_origin(t, win, &win->content_x, &win->content_y);
}

static void p_titlebar(const unoui_theme *t, const unoui_window *win)
{
    unoui_rect r = win->r;
    int th = t->m.title_h, i, tw, tx;
    fb_fill_rect(r.x+2, r.y+2, r.w-4, th-3, MP_WHITE);
    if (win->active)
        for (i = 4; i < th - 2; i += 2)
            fb_hline(r.x + 3, r.y + i, r.w - 6, MP_BLACK);
    /* close box */
    { unoui_rect cb = { r.x + 9, r.y + (th-11)/2, 11, 11 };
      fb_fill_rect(cb.x, cb.y, 11, 11, MP_WHITE);
      if (win->active) fb_frame_rect(cb.x, cb.y, 11, 11, MP_BLACK); }
    tw = fb_text_w(win->title); tx = r.x + (r.w - tw)/2;
    fb_fill_rect(tx - 6, r.y + 2, tw + 12, th - 3, MP_WHITE);
    fb_text(tx, r.y + (th - fb_text_h())/2, win->title, MP_BLACK, -1);
}

static void p_button(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    (void)t;
    if (f & UI_F_DEFAULT) {
        ui_round_frame((unoui_rect){ r.x-4, r.y-4, r.w+8, r.h+8 }, 3, MP_BLACK);
        ui_round_frame((unoui_rect){ r.x-3, r.y-3, r.w+6, r.h+6 }, 3, MP_BLACK);
    }
    ui_round_fill(r, 3, (f & UI_F_PRESSED) ? MP_BLACK : MP_WHITE);
    ui_round_frame(r, 3, MP_BLACK);
    ui_text_in(r, s, (f & UI_F_PRESSED) ? MP_WHITE : MP_BLACK, -1, 1);
}

static const unoui_draw macplus_draw = {
    p_desktop, p_window, p_titlebar, p_button, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0
};

const unoui_theme theme_macplus = {
    "Mac Plus",
    {
        /* desktop  */ MP_WHITE, MP_BLACK,        /* -> 50% stipple via p_desktop */
        /* win bg   */ MP_WHITE,
        /* frame    */ MP_BLACK,
        /* title    */ MP_WHITE, MP_BLACK,
        /* title in */ MP_WHITE, MP_BLACK,
        /* text     */ MP_BLACK, MP_BLACK,
        /* face     */ MP_WHITE, MP_BLACK,
        /* light    */ MP_WHITE, /* shadow */ MP_BLACK, /* dark */ MP_BLACK,
        /* accent   */ MP_BLACK, MP_WHITE,
        /* field    */ MP_WHITE, MP_BLACK
    },
    { /* title_h */ 17, /* frame_w */ 2, /* bevel */ 1, /* pad */ 12,
      /* radius */ 0, /* closebox */ 0, /* shadow */ 2, /* title_center */ 1,
      UNOUI_DEPTH_1 },
    &macplus_draw
};

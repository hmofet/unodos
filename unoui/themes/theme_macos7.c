/* theme_macos7 - Mac OS System 7. GRAPHICS theming: the signature horizontal
 * "racing stripe" pinstripe title bar with a square close box (left) and zoom
 * box (right), rounded white windows with a hard drop shadow, and rounded
 * 1px-outline buttons with the classic bold default ring. */
#include "../unoui_theme.h"

#define M7_BLACK FB_RGB(0x00,0x00,0x00)
#define M7_WHITE FB_RGB(0xFF,0xFF,0xFF)
#define M7_GREY  FB_RGB(0x88,0x88,0x88)
#define M7_LTGRY FB_RGB(0xCC,0xCC,0xCC)

static void m_window(const unoui_theme *t, unoui_window *win)
{
    unoui_rect r = win->r, body;
    /* hard drop shadow (offset solid black, classic System 7) */
    ui_round_fill((unoui_rect){ r.x+3, r.y+3, r.w, r.h }, t->m.radius, M7_GREY);
    ui_round_fill(r, t->m.radius, M7_WHITE);
    ui_round_frame(r, t->m.radius, M7_BLACK);
    /* line under the title bar */
    fb_hline(r.x+1, r.y + t->m.title_h, r.w-2, M7_BLACK);
    body = (unoui_rect){ r.x+1, r.y + t->m.title_h + 1, r.w-2, r.h - t->m.title_h - 2 };
    fb_fill_rect(body.x, body.y, body.w, body.h, M7_WHITE);
    unoui_content_origin(t, win, &win->content_x, &win->content_y);
}

static void m_titlebar(const unoui_theme *t, const unoui_window *win)
{
    unoui_rect r = win->r;
    int th = t->m.title_h, i, tw, tx;
    if (win->active) {                              /* 6 horizontal pinstripes */
        for (i = 3; i < th - 2; i += 2)
            fb_hline(r.x + 2, r.y + i, r.w - 4, M7_BLACK);
    }
    /* close box (left) and zoom box (right): white square, black outline */
    { unoui_rect cb = { r.x + 8, r.y + (th-11)/2, 11, 11 };
      fb_fill_rect(cb.x, cb.y, 11, 11, M7_WHITE);
      if (win->active) fb_frame_rect(cb.x, cb.y, 11, 11, M7_BLACK); }
    { unoui_rect zb = { r.x + r.w - 19, r.y + (th-11)/2, 11, 11 };
      fb_fill_rect(zb.x, zb.y, 11, 11, M7_WHITE);
      if (win->active) { fb_frame_rect(zb.x, zb.y, 11, 11, M7_BLACK);
                         fb_frame_rect(zb.x+2, zb.y+2, 5, 5, M7_BLACK); } }
    /* title on a cleared white plaque, centred */
    tw = fb_text_w(win->title);
    tx = r.x + (r.w - tw) / 2;
    fb_fill_rect(tx - 6, r.y + 1, tw + 12, th - 1, M7_WHITE);
    fb_text(tx, r.y + (th - fb_text_h())/2, win->title,
            win->active ? M7_BLACK : M7_GREY, -1);
}

static void m_button(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    (void)t;
    if (f & UI_F_DEFAULT) {                          /* bold rounded ring */
        unoui_rect ring = { r.x-4, r.y-4, r.w+8, r.h+8 };
        ui_round_frame(ring, 3, M7_BLACK);
        ring = (unoui_rect){ r.x-5, r.y-5, r.w+10, r.h+10 };
        ui_round_frame(ring, 3, M7_BLACK);
        ring = (unoui_rect){ r.x-3, r.y-3, r.w+6, r.h+6 };
        ui_round_frame(ring, 3, M7_BLACK);
    }
    if (f & UI_F_PRESSED) ui_round_fill(r, 3, M7_BLACK);
    else                  ui_round_fill(r, 3, M7_WHITE);
    ui_round_frame(r, 3, M7_BLACK);
    ui_text_in(r, s, (f & UI_F_PRESSED) ? M7_WHITE :
               (f & UI_F_DISABLED) ? M7_GREY : M7_BLACK, -1, 1);
}

static void m_check(const unoui_theme *t, unoui_rect r, const char *s, int f)
{
    (void)t;
    fb_fill_rect(r.x, r.y, 12, 12, M7_WHITE);
    fb_frame_rect(r.x, r.y, 12, 12, M7_BLACK);
    if (f & UI_F_CHECKED) {                          /* an X */
        int i; for (i = 2; i < 10; i++) {
            ui_px(r.x+i, r.y+i, M7_BLACK); ui_px(r.x+11-i, r.y+i, M7_BLACK);
        }
    }
    fb_text(r.x + 18, r.y + (r.h - fb_text_h())/2, s, (f & UI_F_DISABLED) ? M7_GREY : M7_BLACK, -1);
}

static const unoui_draw macos7_draw = {
    0, m_window, m_titlebar, m_button, m_check, 0, 0,
    0, 0, 0, 0, 0, 0, 0
};

const unoui_theme theme_macos7 = {
    "Mac OS 7",
    {
        /* desktop  */ M7_LTGRY, FB_RGB(0xB0,0xB0,0xB0),
        /* win bg   */ M7_WHITE,
        /* frame    */ M7_BLACK,
        /* title    */ M7_WHITE, M7_BLACK,
        /* title in */ M7_WHITE, M7_GREY,
        /* text     */ M7_BLACK, M7_GREY,
        /* face     */ M7_WHITE, M7_BLACK,
        /* light    */ M7_WHITE, /* shadow */ M7_GREY, /* dark */ M7_BLACK,
        /* accent   */ M7_BLACK, M7_WHITE,
        /* field    */ M7_WHITE, M7_BLACK
    },
    { /* title_h */ 18, /* frame_w */ 1, /* bevel */ 1, /* pad */ 12,
      /* radius */ 3, /* closebox */ 0, /* shadow */ 3, /* title_center */ 1,
      UNOUI_DEPTH_FULL },
    &macos7_draw
};

/* theme_amiga - Amiga Workbench 1.x. The classic 4-colour set (grey-blue
 * desktop, white/black/orange detail). Mostly palette theming over the default
 * painters; a small titlebar override adds the Workbench drag-bar look with a
 * depth/sizing gadget at the right. */
#include "../unoui_theme.h"

#define A_BLUE   FB_RGB(0x00,0x55,0xAA)
#define A_WHITE  FB_RGB(0xFF,0xFF,0xFF)
#define A_BLACK  FB_RGB(0x00,0x00,0x00)
#define A_ORANGE FB_RGB(0xFF,0x88,0x00)

static void a_titlebar(const unoui_theme *t, const unoui_window *win)
{
    unoui_rect r = win->r;
    int th = t->m.title_h, fw = t->m.frame_w, gw = 18;
    fb_px bg = win->active ? A_WHITE : A_BLUE;
    fb_px fg = win->active ? A_BLACK : A_WHITE;
    fb_fill_rect(r.x+fw, r.y+fw, r.w-2*fw, th-fw, bg);
    fb_hline(r.x+fw, r.y+th-1, r.w-2*fw, A_BLACK);
    /* front/back depth gadget at far right */
    { unoui_rect g = { r.x + r.w - fw - gw, r.y + fw, gw, th - fw };
      fb_fill_rect(g.x, g.y, g.w, g.h, A_ORANGE);
      fb_frame_rect(g.x, g.y, g.w, g.h, A_BLACK);
      fb_frame_rect(g.x+3, g.y+3, gw-9, th-fw-7, A_BLACK); }
    fb_text(r.x + fw + 6, r.y + (th - fb_text_h())/2, win->title, fg, -1);
}

static const unoui_draw amiga_draw = {
    0, 0, a_titlebar, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

const unoui_theme theme_amiga = {
    "Amiga Workbench",
    {
        /* desktop  */ A_BLUE, A_BLUE,
        /* win bg   */ A_BLUE,
        /* frame    */ A_BLACK,
        /* title    */ A_WHITE, A_BLACK,
        /* title in */ A_BLUE, A_WHITE,
        /* text     */ A_WHITE, FB_RGB(0xB0,0xB0,0xB0),
        /* face     */ A_WHITE, A_BLACK,
        /* light    */ A_WHITE, /* shadow */ A_BLACK, /* dark */ A_BLACK,
        /* accent   */ A_ORANGE, A_BLACK,
        /* field    */ A_WHITE, A_BLACK
    },
    { /* title_h */ 18, /* frame_w */ 2, /* bevel */ 1, /* pad */ 10,
      /* radius */ 0, /* closebox */ 10, /* shadow */ 0, /* title_center */ 0,
      UNOUI_DEPTH_4 },
    &amiga_draw
};

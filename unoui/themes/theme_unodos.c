/* theme_unodos - the default unified UnoDOS look. PALETTE-ONLY theming: it
 * sets the semantic colours + metrics and uses the portable default painters
 * (draw = NULL). This is the "write once, looks the same everywhere" baseline -
 * the house blue/cyan style every port shares. */
#include "../unoui_theme.h"

const unoui_theme theme_unodos = {
    "UnoDOS",
    {
        /* desktop  */ FB_RGB(0x00,0x00,0xAA), FB_RGB(0x00,0x00,0x80),
        /* win bg   */ FB_RGB(0xC8,0xC8,0xD0),
        /* frame    */ FB_RGB(0x00,0x00,0x00),
        /* title    */ FB_RGB(0x00,0xAA,0xAA), FB_RGB(0x00,0x00,0x00),
        /* title in */ FB_RGB(0x70,0x90,0x90), FB_RGB(0x20,0x20,0x20),
        /* text     */ FB_RGB(0x00,0x00,0x00), FB_RGB(0x80,0x80,0x88),
        /* face     */ FB_RGB(0xC8,0xC8,0xD0), FB_RGB(0x00,0x00,0x00),
        /* light    */ FB_RGB(0xFF,0xFF,0xFF),
        /* shadow   */ FB_RGB(0x80,0x80,0x88),
        /* dark     */ FB_RGB(0x00,0x00,0x00),
        /* accent   */ FB_RGB(0x00,0xAA,0xAA), FB_RGB(0xFF,0xFF,0xFF),
        /* field    */ FB_RGB(0xFF,0xFF,0xFF), FB_RGB(0x00,0x00,0x00)
    },
    { /* title_h */ 18, /* frame_w */ 2, /* bevel */ 1, /* pad */ 10,
      /* radius */ 0, /* closebox */ 10, /* shadow */ 0, /* title_center */ 1,
      UNOUI_DEPTH_FULL },
    0   /* default painters */
};

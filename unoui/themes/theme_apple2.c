/* theme_apple2 - Apple II green-phosphor monochrome. Two colours only: P1
 * green on black. Palette theming over the default painters (the bevels become
 * green hairlines, exactly like a mono terminal would render a "3D" box). */
#include "../unoui_theme.h"

#define A2_BLACK FB_RGB(0x00,0x00,0x00)
#define A2_GRN   FB_RGB(0x33,0xFF,0x33)
#define A2_DIM   FB_RGB(0x11,0x80,0x11)

const unoui_theme theme_apple2 = {
    "Apple II",
    {
        /* desktop  */ A2_BLACK, A2_BLACK,
        /* win bg   */ A2_BLACK,
        /* frame    */ A2_GRN,
        /* title    */ A2_BLACK, A2_GRN,
        /* title in */ A2_BLACK, A2_DIM,
        /* text     */ A2_GRN, A2_DIM,
        /* face     */ A2_BLACK, A2_GRN,
        /* light    */ A2_GRN, /* shadow */ A2_DIM, /* dark */ A2_GRN,
        /* accent   */ A2_GRN, A2_BLACK,
        /* field    */ A2_BLACK, A2_GRN
    },
    { /* title_h */ 16, /* frame_w */ 1, /* bevel */ 1, /* pad */ 12,
      /* radius */ 0, /* closebox */ 0, /* shadow */ 0, /* title_center */ 1,
      UNOUI_DEPTH_1 },
    0   /* default painters */
};

/* theme_c64 - Commodore 64. Pure palette theming over the default painters:
 * the unmistakable two-blue VIC-II screen (light-blue text on a darker blue
 * ground, light-blue border). No graphics overrides needed - proof that a
 * radically different platform identity can come from colours alone. */
#include "../unoui_theme.h"

#define C_BORDER FB_RGB(0x6C,0x5E,0xB5)   /* VIC light blue (border)   */
#define C_BG     FB_RGB(0x35,0x28,0x79)   /* VIC blue (screen)         */
#define C_FG     FB_RGB(0x6C,0x5E,0xB5)   /* light blue text           */
#define C_WHITE  FB_RGB(0xFF,0xFF,0xFF)
#define C_CYAN   FB_RGB(0x70,0xA4,0xB2)

const unoui_theme theme_c64 = {
    "Commodore 64",
    {
        /* desktop  */ C_BORDER, C_BORDER,
        /* win bg   */ C_BG,
        /* frame    */ C_FG,
        /* title    */ C_BG, C_FG,
        /* title in */ C_BG, FB_RGB(0x44,0x3C,0x8C),
        /* text     */ C_FG, FB_RGB(0x50,0x48,0x90),
        /* face     */ C_BG, C_FG,
        /* light    */ C_FG, /* shadow */ FB_RGB(0x20,0x18,0x50), /* dark */ FB_RGB(0x10,0x0C,0x30),
        /* accent   */ C_CYAN, C_BG,
        /* field    */ C_BG, C_WHITE
    },
    { /* title_h */ 16, /* frame_w */ 3, /* bevel */ 1, /* pad */ 12,
      /* radius */ 0, /* closebox */ 0, /* shadow */ 0, /* title_center */ 1,
      UNOUI_DEPTH_4 },
    0   /* default painters */
};

/* theme_next - NeXTSTEP. The chiselled greyscale look: a medium-grey desktop,
 * light-grey windows, black title text on grey, and 2px bevels that read as the
 * NeXT "chisel". Mostly palette + metrics over the default painters (the wider
 * bevel does the chiselling). depth 8 = smooth greys. */
#include "../unoui_theme.h"

#define N_DKGRY  FB_RGB(0x55,0x55,0x55)
#define N_GREY   FB_RGB(0xAA,0xAA,0xAA)
#define N_LTGRY  FB_RGB(0xDD,0xDD,0xDD)
#define N_WHITE  FB_RGB(0xFF,0xFF,0xFF)
#define N_BLACK  FB_RGB(0x00,0x00,0x00)

const unoui_theme theme_next = {
    "NeXTSTEP",
    {
        /* desktop  */ N_DKGRY, FB_RGB(0x4A,0x4A,0x4A),
        /* win bg   */ N_GREY,
        /* frame    */ N_BLACK,
        /* title    */ N_GREY, N_BLACK,
        /* title in */ N_DKGRY, N_LTGRY,
        /* text     */ N_BLACK, FB_RGB(0x66,0x66,0x66),
        /* face     */ N_GREY, N_BLACK,
        /* light    */ N_WHITE, /* shadow */ FB_RGB(0x55,0x55,0x55), /* dark */ N_BLACK,
        /* accent   */ N_BLACK, N_WHITE,
        /* field    */ N_WHITE, N_BLACK
    },
    { /* title_h */ 20, /* frame_w */ 2, /* bevel */ 2, /* pad */ 12,
      /* radius */ 0, /* closebox */ 12, /* shadow */ 0, /* title_center */ 1,
      UNOUI_DEPTH_8 },
    0   /* default painters */
};

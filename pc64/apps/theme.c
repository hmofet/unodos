/* Theme app module (APP_THEME).  Separate artifact -> app10.so.
   Faithful port of theme_draw/theme_key from unodos.c. */
#include "uno_mod.h"

#define NTHEMES 8
static const char *kThemeNames[NTHEMES] = {
    "Classic VGA", "Midnight", "Forest", "Sunset",
    "Ocean", "Slate", "Candy", "Amber"
};
static const RGBColor kThemes[NTHEMES][4] = {
  {{0x0000,0x0000,0xAAAA},{0x0000,0xAAAA,0xAAAA},{0xAAAA,0x0000,0xAAAA},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x0000,0x0000,0x0000},{0x5555,0x5555,0xFFFF},{0xAAAA,0xAAAA,0xAAAA},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x0000,0x5555,0x0000},{0x5555,0xAAAA,0x5555},{0xFFFF,0xFFFF,0x5555},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x5555,0x0000,0x0000},{0xFFFF,0x5555,0x5555},{0xFFFF,0xAAAA,0x0000},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x0000,0x0000,0x5555},{0x0000,0x8888,0xAAAA},{0x5555,0xFFFF,0xFFFF},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x3333,0x3333,0x4444},{0x8888,0x8888,0xAAAA},{0xCCCC,0xCCCC,0xDDDD},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x5555,0x0000,0x5555},{0xFFFF,0x5555,0xFFFF},{0x5555,0xFFFF,0xFFFF},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x0000,0x0000,0x0000},{0xAAAA,0x5555,0x0000},{0xFFFF,0xAAAA,0x0000},{0xFFFF,0xFFFF,0xFFFF}},
};
static short gTSel = 0, gTSlot = 0;

static void theme_draw(UnoWin *w)
{
    Rect r = w->bounds, row;
    short x = r.left + 10, y0 = r.top + TBAR_H + 6, i;
    char line[48], num[12];
    for (i = 0; i < NTHEMES; i++) {
        short ry = y0 + i * 16;
        Boolean sel = (i == gTSel);
        SetRect(&row, r.left + 4, ry, r.right - 4, ry + 16);
        if (sel) uno_fill(&row, C_CYAN);
        text_at(x, ry + 12, kThemeNames[i], sel ? C_BLUE : C_WHITE,
                sel ? C_CYAN : C_BLUE, false);
    }
    {
        short cy = y0 + NTHEMES * 16 + 14;
        const RGBColor *c = &kPalette[gTSlot];
        strcpy(line, "Custom  Slot ");
        fmt_u(gTSlot, num); strcat(line, num);
        strcat(line, "   R/G/B  ");
        fmt_u(c->red >> 12, num);   strcat(line, num); strcat(line, "/");
        fmt_u(c->green >> 12, num); strcat(line, num); strcat(line, "/");
        fmt_u(c->blue >> 12, num);  strcat(line, num);
        text_at(x, cy, line, C_WHITE, C_BLUE, false);
        text_at(x, cy + 16, "Enter: apply   r/g/b: tune   </>: slot",
                C_CYAN, C_BLUE, false);
    }
}

static void theme_tune(short chan)
{
    RGBColor *c = &kPalette[gTSlot];
    unsigned short *v = (chan == 0) ? &c->red : (chan == 1) ? &c->green : &c->blue;
    *v = (unsigned short)((((*v >> 12) + 1) & 15) * 0x1111);
    repaint_all();
}

static Boolean theme_key(char ch, short code, Boolean cmd)
{
    UnoWin *w = find_app_window(APP_THEME);
    if (cmd) return false;
    if (code == 0x7D || ch == 0x1F) { if (gTSel < NTHEMES - 1) gTSel++; if (w) draw_window(w); return true; }
    if (code == 0x7E || ch == 0x1E) { if (gTSel > 0) gTSel--; if (w) draw_window(w); return true; }
    if (code == 0x7B || ch == 0x1C) { gTSlot = (gTSlot + 3) & 3; if (w) draw_window(w); return true; }
    if (code == 0x7C || ch == 0x1D) { gTSlot = (gTSlot + 1) & 3; if (w) draw_window(w); return true; }
    if (ch == 0x0D || ch == 0x03) { memcpy(kPalette, kThemes[gTSel], sizeof(RGBColor)*4); repaint_all(); return true; }
    if (ch == 'r' || ch == 'R') { theme_tune(0); return true; }
    if (ch == 'g' || ch == 'G') { theme_tune(1); return true; }
    if (ch == 'b' || ch == 'B') { theme_tune(2); return true; }
    return false;
}

static const AppInterface kIface = {
    theme_draw, theme_key, 0, 0, 0, 0,
    "Theme", { 150, 56, 430, 300 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }

/* Files app module (APP_FILES). Separate artifact -> app02.so.
   Lists the PC (FAT12) volume through the KernelApi storage callbacks - the
   cross-platform storage path.  (HFS PBGetCatInfo browsing stays in the full
   kernel build; the module uses the portable FAT surface exported in KernelApi.) */
#include "uno_mod.h"

#define FROWS  11
#define FROW_H 16

static short gFSel = 0, gFTop = 0;
static Boolean gMounted = false;

static void files_draw(UnoWin *w)
{
    Rect r = w->bounds, row;
    short x = r.left + 8, y0 = r.top + TBAR_H + 4, i;
    char line[64], num[16];
    short count = gFatCount;

    text_at(x, y0 + 10, "Name", C_CYAN, C_BLUE, false);
    text_at(r.right - 80, y0 + 10, "Size", C_CYAN, C_BLUE, false);
    text_at(r.right - 150, y0 + 10, "(PC disk)", C_MAG, C_BLUE, false);

    if (!gMounted)
        text_at(x, y0 + 26, "press V to mount the PC volume", C_WHITE, C_BLUE, false);
    else if (count == 0)
        text_at(x, y0 + 26, "no files on the PC volume", C_WHITE, C_BLUE, false);

    for (i = 0; i < FROWS; i++) {
        short fi = gFTop + i;
        short ry = y0 + 14 + i * FROW_H;
        Boolean sel;
        SetRect(&row, r.left + 2, ry, r.right - 2, ry + FROW_H);
        if (fi >= count) break;
        sel = (fi == gFSel);
        if (sel) uno_fill(&row, C_CYAN);
        strcpy(line, (const char *)gFatNames[fi]);
        text_at_max(x, ry + 12, line, sel ? C_BLUE : C_WHITE, r.right - r.left - 100);
        fmt_u(gFatSizes[fi], num);
        text_at(r.right - 80, ry + 12, num, sel ? C_BLUE : C_WHITE, C_BLUE, false);
    }
    text_at(x, r.bottom - 6, "V: mount   Up/Down: select   R: refresh", C_CYAN, C_BLUE, false);
}

static Boolean files_key(char ch, short code, Boolean cmd)
{
    UnoWin *w = find_app_window(APP_FILES);
    short count = gFatCount;
    if (cmd) return false;
    if (ch == 'v' || ch == 'V') {
        if (fat12_mount()) { gMounted = true; fat12_list(); gFSel = 0; gFTop = 0; }
        if (w) draw_window(w); return true;
    }
    if (code == 0x7D || ch == 0x1F) { if (gFSel < count-1) gFSel++; if (gFSel>=gFTop+FROWS) gFTop=gFSel-FROWS+1; if(w) draw_window(w); return true; }
    if (code == 0x7E || ch == 0x1E) { if (gFSel > 0) gFSel--; if (gFSel<gFTop) gFTop=gFSel; if(w) draw_window(w); return true; }
    if (ch == 'r' || ch == 'R') { fat12_list(); if(w) draw_window(w); return true; }
    return false;
}

static void files_opened(void){ if (fat12_mount()) { gMounted = true; fat12_list(); } }

static const AppInterface kIface = {
    files_draw, files_key, 0, 0, files_opened, 0,
    "Files", { 36, 40, 330, 270 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }

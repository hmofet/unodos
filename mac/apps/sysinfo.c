/* SysInfo app module (APP_SYSINFO).  Separate artifact -> app00.so. */
#include "uno_mod.h"

static void sysinfo_draw(UnoWin *w)
{
    short x = w->bounds.left + 8;
    short y = w->bounds.top + TBAR_H + 14;
    char num[16], line[24];
    text_at(x, y, "Video", C_WHITE, C_BLUE, false);
    text_at(x + 80, y, "Color QuickDraw", C_CYAN, C_BLUE, false);
    text_at(x, y + 16, "System", C_WHITE, C_BLUE, false);
    text_at(x + 80, y + 16, "7.x (Mac II+)", C_CYAN, C_BLUE, false);
    text_at(x, y + 32, "Uptime", C_WHITE, C_BLUE, false);
    fmt_u(now_secs(), num); strcpy(line, num); strcat(line, "s   ");
    text_at(x + 80, y + 32, line, C_CYAN, C_BLUE, true);
    text_at(x, y + 54, "App loaded as a MODULE", C_MAG, C_BLUE, false);
}

static const AppInterface kIface = {
    sysinfo_draw, 0, 0, 0, 0, 0,
    "System Info", { 40, 50, 320, 170 }
};

const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }

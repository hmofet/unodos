/* Clock app module (APP_CLOCK).  Separate artifact -> app01.so.
   Verbatim port of the core's clock_draw over the KernelApi surface. */
#include "uno_mod.h"

static void clock_draw(UnoWin *w)
{
    long s = now_secs();
    char buf[12];
    short cx, cy;
    put2(s / 3600, buf);          buf[2] = ':';
    put2((s / 60) % 60, buf + 3); buf[5] = ':';
    put2(s % 60, buf + 6);        buf[8] = 0;
    text_at(w->bounds.left + 8, w->bounds.top + TBAR_H + 14, "Uptime", C_WHITE, C_BLUE, false);
    cx = w->bounds.left + (w->bounds.right - w->bounds.left) / 2 - 28;
    cy = w->bounds.top + (w->bounds.bottom - w->bounds.top) / 2 + 8;
    text_at(cx, cy, buf, C_CYAN, C_BLUE, true);
}

/* a tick so the uptime display advances while the window is open. Gated on the
   displayed value (now_secs()) actually changing: without this the main loop's
   ungated spin redraws the whole window thousands of times/sec for a value that
   ticks once a second (AUDIT-mac §1 P1) - pure waste + a flicker source. Every
   other app's tick is already TickCount-gated. */
static void clock_tick(void)
{
    static long last = -1;
    long s = now_secs();
    UnoWin *w;
    if (s == last) return;
    last = s;
    w = find_app_window(APP_CLOCK);
    if (w) draw_window(w);
}

static const AppInterface kIface = {
    clock_draw, 0, 0, clock_tick, 0, 0,
    "Clock", { 120, 80, 320, 180 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }

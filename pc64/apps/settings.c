/* Settings app module (APP_SETTINGS) - pc64's first port-specific app.
 *
 * Display-resolution picker, the C-core sibling of the x86 reference port's
 * Settings video-mode selector (CGA/VGA/Mode12h/VESA). The list comes from
 * the platform through the KernelApi's pc64 tail (display_res_*): standard
 * desktop resolutions filtered to what the active GOP mode can carry, each
 * applied at its best integer zoom and centred. On other ports those tail
 * pointers are NULL and this module simply isn't in the app set.
 */
#include "uno_mod.h"

static short gRSel = 0;

static void fmt_res(char *out, short w, short h, short zoom, Boolean active)
{
    char num[12];
    out[0] = 0;
    strcat(out, active ? "* " : "  ");
    fmt_u(w, num); strcat(out, num);
    strcat(out, "x");
    fmt_u(h, num); strcat(out, num);
    if (zoom > 1) {
        strcat(out, "  @");
        fmt_u(zoom, num); strcat(out, num);
        strcat(out, "x");
    }
}

static void settings_draw(UnoWin *w)
{
    Rect r = w->bounds, row;
    short x = r.left + 10, y0 = r.top + TBAR_H + 6, i, n;
    char line[40];

    text_at(x, y0 + 10, "Display resolution", C_MAG, C_BLUE, false);
    if (!display_res_count) {
        text_at(x, y0 + 30, "(not supported on this port)", C_WHITE, C_BLUE, false);
        return;
    }
    n = (short)display_res_count();
    if (gRSel >= n) gRSel = (short)(n - 1);
    for (i = 0; i < n; i++) {
        short ry = (short)(y0 + 18 + i * 14), zoom;
        short rw, rh;
        Boolean sel = (i == gRSel), active;
        display_res_get(i, &rw, &rh, &zoom, &active);
        fmt_res(line, rw, rh, zoom, active);
        SetRect(&row, (short)(r.left + 4), ry, (short)(r.right - 4), (short)(ry + 14));
        if (sel) uno_fill(&row, C_CYAN);
        text_at(x, (short)(ry + 11), line, sel ? C_BLUE : C_WHITE,
                sel ? C_CYAN : C_BLUE, false);
    }
    text_at(x, (short)(r.bottom - 6), "Up/Down: select   Enter: apply",
            C_CYAN, C_BLUE, false);
}

static void settings_apply(void)
{
    if (display_res_set) display_res_set(gRSel);
    /* the platform repaints everything via the resolution-change hook */
}

static Boolean settings_key(char ch, short code, Boolean cmd)
{
    UnoWin *w = find_app_window(APP_SETTINGS);
    short n = display_res_count ? (short)display_res_count() : 0;
    if (cmd || !n) return false;
    if (code == 0x7D || ch == 0x1F) { if (gRSel < n - 1) gRSel++; if (w) draw_window(w); return true; }
    if (code == 0x7E || ch == 0x1E) { if (gRSel > 0) gRSel--; if (w) draw_window(w); return true; }
    if (ch == 0x0D || ch == 0x03) { settings_apply(); return true; }
    return false;
}

static void settings_click(UnoWin *w, Point p)
{
    short y0 = (short)(w->bounds.top + TBAR_H + 6), n, i;
    if (!display_res_count) return;
    n = (short)display_res_count();
    i = (short)((p.v - (y0 + 18)) / 14);
    if (i < 0 || i >= n) return;
    if (i == gRSel) settings_apply();           /* click again = apply */
    else { gRSel = i; draw_window(w); }
}

static const AppInterface kIface = {
    settings_draw, settings_key, settings_click, 0, 0, 0,
    "Settings", { 140, 50, 480, 336 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }

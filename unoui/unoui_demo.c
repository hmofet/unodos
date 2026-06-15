/* ===========================================================================
 * unoui_demo - the WRITE-ONCE UI. This function builds one window full of
 * widgets exactly once. The harness then renders this same tree under every
 * theme without touching it again - that is the whole proof of the toolkit:
 * one app description, many native looks.
 * ===========================================================================
 */
#include "unoui_demo.h"

static const char *g_files[] = {
    "README.TXT", "KERNEL.BIN", "UNOUI.DOC", "THEMES.DAT", "BOOT.SYS"
};

void demo_build(unoui_window *win, int screen_w, int screen_h)
{
    int x = (screen_w - DEMO_W) / 2;
    int y = (screen_h - DEMO_H) / 2;
    unoui_window_init(win, "UnoDOS Control Panel", x, y, DEMO_W, DEMO_H);

    /* --- left column: a couple of group boxes ------------------------------ */
    unoui_add_group(win, 0, 0, 150, 72, "Resolution");
    unoui_add_radio(win, 10, 16, "320 x 200", 0);
    unoui_add_radio(win, 10, 34, "640 x 480", 1);
    unoui_add_radio(win, 10, 52, "800 x 600", 0);

    unoui_add_group(win, 0, 84, 150, 60, "Options");
    unoui_add_check(win, 10, 100, "Sound",      1);
    unoui_add_check(win, 10, 120, "Full screen", 0);

    unoui_add_label(win, 0, 158, "Volume");
    unoui_add_progress(win, 0, 170, 150, 65, 100);

    unoui_add_label(win, 0, 192, "Name:");
    unoui_add_field(win, 44, 188, 106, "UnoDOS", 1);

    /* --- right column: a file list + scrollbar + buttons ------------------- */
    unoui_add_label(win, 170, 0, "Files:");
    unoui_add_list (win, 170, 12, 122, 100, g_files, 5, 2);
    unoui_add_vscroll(win, 296, 12, 100, 20, 80);

    unoui_add_sep(win, 170, 124, 156);

    unoui_add_button(win, 170, 138, 72, "Cancel", 0);
    unoui_add_button(win, 252, 138, 72, "OK",     UI_F_DEFAULT);
}

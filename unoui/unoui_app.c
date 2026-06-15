/* The interactive write-once demo app. Built once; driven by events on every
 * platform. See unoui_app.h. */
#include "unoui_app.h"

/* app-owned editable buffers */
static char g_body[512];
static char g_name[64];
static unoui_text g_body_t, g_name_t;

static const char *g_files[]  = { "boot.sys", "kernel.bin", "unoui.c",
                                  "themes.dat", "readme.txt", "config.ini",
                                  "splash.raw" };
static const char *g_format[] = { "Plain text", "Markdown", "RTF", "HTML" };
static const char *g_tabs[]   = { "Edit", "Files", "About" };

static const char *m_file[] = { "New", "Open...", "Save", "Quit" };
static const char *m_edit[] = { "Undo", "Cut", "Copy", "Paste" };
static const char *m_view[] = { "Zoom In", "Zoom Out", "Full Screen" };
static const unoui_menu g_menus[] = {
    { "File", m_file, 4 }, { "Edit", m_edit, 4 }, { "View", m_view, 3 }
};

void demo_app_build(unoui_window *ed, unoui_window *pal)
{
    unoui_widget *w;

    /* ---- editor window --------------------------------------------------- */
    unoui_window_init(ed, "unoui demo - Editor", 26, 24, 388, 372);
    unoui_text_init(&g_body_t, g_body, sizeof g_body, 1);   /* multi-line */
    unoui_text_set(&g_body_t, "");
    unoui_text_init(&g_name_t, g_name, sizeof g_name, 0);   /* single line */
    unoui_text_set(&g_name_t, "untitled");

    w = unoui_add_menubar(ed, g_menus, 3);                  w->id = ID_MENU;
    w = unoui_add_tabs(ed, 0, 12, 356, g_tabs, 3, 0);       w->id = ID_TABS;

    unoui_add_label(ed, 0, 40, "Document body:");
    w = unoui_add_textarea(ed, 0, 52, 320, 96, &g_body_t);  w->id = ID_BODY;
    unoui_add_vscroll(ed, 324, 52, 96, 30, 120);

    unoui_add_label(ed, 0, 158, "Name:");
    w = unoui_add_edit(ed, 44, 154, 180, &g_name_t);        w->id = ID_NAME;
    unoui_add_label(ed, 234, 158, "Fmt:");
    w = unoui_add_dropdown(ed, 268, 154, 90, g_format, 4, 0); w->id = ID_FORMAT;

    unoui_add_label(ed, 0, 182, "Volume");
    w = unoui_add_slider(ed, 56, 178, 168, 0, 100, 60);     w->id = ID_VOL;
    unoui_add_label(ed, 234, 182, "Copies");
    w = unoui_add_spinner(ed, 290, 178, 64, 1, 99, 1);      w->id = ID_COUNT;

    unoui_add_check(ed, 0, 204, "Word wrap",  1);           ed->w[ed->nw-1].id = ID_WRAP;
    unoui_add_check(ed, 110, 204, "Dark mode", 0);          ed->w[ed->nw-1].id = ID_DARK;

    unoui_add_label(ed, 234, 200, "Files:");
    w = unoui_add_list(ed, 234, 212, 124, 92, g_files, 7, 1); w->id = ID_FILES;

    unoui_add_label(ed, 0, 222, "Scroll:");
    unoui_add_hscroll(ed, 0, 234, 210, 40, 100);

    unoui_add_sep(ed, 0, 256, 220);
    w = unoui_add_button(ed, 0,  300, 100, "Cancel", 0);    w->id = ID_CANCEL;
    w = unoui_add_button(ed, 120, 300, 100, "OK", UI_F_DEFAULT); w->id = ID_OK;

    /* ---- palette window (small; demonstrates z-order + dragging) --------- */
    unoui_window_init(pal, "Tools", 438, 70, 168, 150);
    unoui_add_group(pal, 0, 0, 144, 78, "Tool");
    unoui_add_radio(pal, 10, 16, "Pen",   1);
    unoui_add_radio(pal, 10, 34, "Fill",  0);
    unoui_add_radio(pal, 10, 52, "Eraser",0);
    w = unoui_add_button(pal, 0, 92, 144, "Apply", UI_F_DEFAULT); w->id = ID_APPLY;
}

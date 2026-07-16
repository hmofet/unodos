/* ===========================================================================
 * UnoDOS/pc64 - the unoui SHELL. An alternative main() (built with -DUNO_UUI
 * in place of unodos.c) that makes the CROSS-PLATFORM unoui toolkit the whole
 * UI: a themed desktop + window manager + retained-mode widgets, driven by the
 * same UEFI keyboard/mouse the rest of the port already produces.
 *
 * unoui owns the windows, focus, dragging, menus, popups and every widget; it
 * renders into the software framebuffer `fb`, which uno_pc64_present() scales
 * to the panel. This file is only: (1) the event adapter (UEFI input ->
 * unoui_event), (2) the window/widget tree, and (3) the action handlers
 * (live theme + resolution switching, text editing, list/slider/etc.). Every
 * control is reachable by KEYBOARD (Tab/arrows/Enter) as well as the pointer,
 * so it is usable with no mouse (and verifiable headlessly).
 * ======================================================================== */
#include "unoui.h"
#include "unoui_theme.h"     /* the theme structs + theme_* externs */
#include "mac_compat.h"      /* FB_W/FB_H + uno_pc64_* + FSOpen/... */
#include <string.h>

/* ---- theme table (the dropdown + live re-skin) -------------------------- */
static const struct {
    const char *name;
    const struct unoui_theme *theme;
} kThemes[] = {
    { "UnoDOS",    &theme_unodos  }, { "Mac OS 7",   &theme_macos7 },
    { "Mac Plus",  &theme_macplus }, { "Windows 3.1",&theme_win31  },
    { "Amiga",     &theme_amiga   }, { "C64",        &theme_c64    },
    { "Apple II",  &theme_apple2  }, { "NeXTSTEP",   &theme_next   }
};
#define NTHEMES ((int)(sizeof kThemes / sizeof kThemes[0]))
static const char *kThemeNames[NTHEMES];

/* ---- widget ids --------------------------------------------------------- */
enum { ID_THEME = 1, ID_RES, ID_DARK, ID_WRAP, ID_VOL, ID_SCALE, ID_ABOUT,
       ID_MENU, ID_BODY, ID_NAME, ID_SAVE, ID_OPEN, ID_NEWF, ID_FILES, ID_FMT };

static unoui_ui      UI;
static unoui_window  WCTRL, WEDIT, WFILES, WSYS;

/* editor buffers */
static char g_body[1024], g_name[32];
static unoui_text g_body_t, g_name_t;
static char g_status[48] = "unoui shell ready";

/* resolution dropdown items (filled from the platform) */
static char  g_res_str[12][14];
static const char *g_res_items[12];
static int   g_res_n;

static const char *g_files[] = { "README.TXT", "NOTES.TXT", "SONG.TRK" };
static const char *g_fmts[]  = { "Plain", "Markdown", "UnoDOS" };

/* ---- Pascal-string helper for the RAM-disk File Manager ----------------- */
static void c2p(const char *s, unsigned char *p)
{ int n = (int)strlen(s); if (n > 31) n = 31; p[0] = (unsigned char)n;
  { int i; for (i = 0; i < n; i++) p[i + 1] = (unsigned char)s[i]; } }

static void editor_save(void)
{
    unsigned char pn[34]; short ref; long n = g_body_t.len;
    c2p(g_name[0] ? g_name : "NOTES.TXT", pn);
    Create(pn, 0, 0, 0);
    if (FSOpen(pn, 0, &ref) == noErr) {
        FSWrite(ref, &n, g_body); FSClose(ref);
        strcpy(g_status, "saved ");
        { int l = (int)strlen(g_status), i = 0; while (g_name[i] && l < 40) g_status[l++] = g_name[i++]; g_status[l] = 0; }
    } else strcpy(g_status, "save failed");
}
static void editor_open(void)
{
    unsigned char pn[34]; short ref; long n = sizeof g_body - 1;
    c2p(g_name[0] ? g_name : "NOTES.TXT", pn);
    if (FSOpen(pn, 0, &ref) == noErr) {
        if (FSRead(ref, &n, g_body) == noErr && n >= 0) { g_body[n] = 0;
            unoui_text_set(&g_body_t, g_body); strcpy(g_status, "opened"); }
        FSClose(ref);
    } else strcpy(g_status, "not found");
}

/* ---- build the resolution dropdown from the platform -------------------- */
static void build_res_items(void)
{
    int i, n = uno_pc64_res_count();
    if (n > 12) n = 12;
    for (i = 0; i < n; i++) {
        short w, h, z; Boolean act; char *o = g_res_str[i]; int j = 0;
        char num[8];
        uno_pc64_res_get(i, &w, &h, &z, &act);
        { int v = w, k = 0; char t[8]; if (!v) t[k++] = '0'; while (v) { t[k++] = '0' + v % 10; v /= 10; }
          while (k) o[j++] = t[--k]; }
        o[j++] = 'x';
        { int v = h, k = 0; char t[8]; if (!v) t[k++] = '0'; while (v) { t[k++] = '0' + v % 10; v /= 10; }
          while (k) o[j++] = t[--k]; }
        o[j] = 0; (void)num;
        g_res_items[i] = o;
    }
    g_res_n = n;
}

/* ---- menus -------------------------------------------------------------- */
static const char *m_file[] = { "New", "Open", "Save" };
static const char *m_edit[] = { "Cut", "Copy", "Paste" };
static const unoui_menu g_menus[] = { { "File", m_file, 3 }, { "Edit", m_edit, 3 } };

static void build_windows(void)
{
    unoui_widget *w;
    int i;
    for (i = 0; i < NTHEMES; i++) kThemeNames[i] = kThemes[i].name;
    build_res_items();

    /* ---- Control Panel -------------------------------------------------- */
    unoui_window_init(&WCTRL, "Control Panel", 16, 24, 250, 210);
    unoui_add_label(&WCTRL, 8, 8, "Theme:");
    w = unoui_add_dropdown(&WCTRL, 70, 4, 150, kThemeNames, NTHEMES, 0); w->id = ID_THEME;
    unoui_add_label(&WCTRL, 8, 32, "Resolution:");
    w = unoui_add_dropdown(&WCTRL, 90, 28, 130, g_res_items, g_res_n, 0); w->id = ID_RES;
    unoui_add_check(&WCTRL, 8, 58, "Dark mode", 0);  WCTRL.w[WCTRL.nw-1].id = ID_DARK;
    unoui_add_check(&WCTRL, 120, 58, "Word wrap", 1); WCTRL.w[WCTRL.nw-1].id = ID_WRAP;
    unoui_add_label(&WCTRL, 8, 84, "Volume");
    w = unoui_add_slider(&WCTRL, 66, 80, 154, 0, 100, 70); w->id = ID_VOL;
    unoui_add_label(&WCTRL, 8, 108, "UI scale");
    w = unoui_add_spinner(&WCTRL, 72, 104, 70, 1, 4, 1); w->id = ID_SCALE;
    w = unoui_add_button(&WCTRL, 8, 140, 100, "About", 0); w->id = ID_ABOUT;
    unoui_add_sep(&WCTRL, 8, 172, 214);
    unoui_add_label(&WCTRL, 8, 178, "Tab/arrows/Enter or the pointer");

    /* ---- Editor --------------------------------------------------------- */
    unoui_window_init(&WEDIT, "Editor", 280, 24, 300, 260);
    unoui_text_init(&g_body_t, g_body, sizeof g_body, 1);
    unoui_text_set(&g_body_t, "unoui is now the whole UnoDOS UI.\n"
                              "Windows, menus, widgets, themes -\n"
                              "one toolkit, driven by keyboard or\n"
                              "the pointer. Edit me, then Save.");
    unoui_text_init(&g_name_t, g_name, sizeof g_name, 0);
    unoui_text_set(&g_name_t, "NOTES.TXT");
    w = unoui_add_menubar(&WEDIT, g_menus, 2); w->id = ID_MENU;
    unoui_add_label(&WEDIT, 4, 30, "Body:");
    w = unoui_add_textarea(&WEDIT, 4, 42, 262, 120, &g_body_t); w->id = ID_BODY;
    unoui_add_vscroll(&WEDIT, 268, 42, 120, 0, 100);
    unoui_add_label(&WEDIT, 4, 170, "File:");
    w = unoui_add_edit(&WEDIT, 40, 166, 130, &g_name_t); w->id = ID_NAME;
    unoui_add_label(&WEDIT, 178, 170, "Fmt:");
    w = unoui_add_dropdown(&WEDIT, 210, 166, 78, g_fmts, 3, 2); w->id = ID_FMT;
    w = unoui_add_button(&WEDIT, 4,  196, 80, "Save", UI_F_DEFAULT); w->id = ID_SAVE;
    w = unoui_add_button(&WEDIT, 90, 196, 80, "Open", 0); w->id = ID_OPEN;
    w = unoui_add_button(&WEDIT, 176, 196, 80, "New", 0); w->id = ID_NEWF;
    unoui_add_label(&WEDIT, 4, 228, g_status);

    /* ---- Files ---------------------------------------------------------- */
    unoui_window_init(&WFILES, "Files", 16, 250, 180, 150);
    unoui_add_label(&WFILES, 6, 6, "RAM disk:");
    w = unoui_add_list(&WFILES, 6, 20, 156, 100, g_files, 3, 0); w->id = ID_FILES;

    /* ---- System info ---------------------------------------------------- */
    unoui_window_init(&WSYS, "System", 280, 300, 300, 96);
    unoui_add_label(&WSYS, 8, 6,  "UnoDOS / pc64  -  unoui shell");
    unoui_add_label(&WSYS, 8, 24, "x86-64 UEFI  -  bare metal");
    unoui_add_label(&WSYS, 8, 42, "8 themes  -  live re-skin");
    unoui_add_label(&WSYS, 8, 60, "TrackPoint / touchpad + keyboard");

    unoui_ui_add(&UI, &WSYS);
    unoui_ui_add(&UI, &WFILES);
    unoui_ui_add(&UI, &WEDIT);
    unoui_ui_add(&UI, &WCTRL);     /* topmost / focused */
}

/* ---- keep windows on-screen after a resolution change ------------------- */
static void reflow(void)
{
    int i;
    UI.screen_w = FB_W; UI.screen_h = FB_H;
    for (i = 0; i < UI.nwin; i++) {
        unoui_window *win = UI.win[i];
        if (win->r.x + win->r.w > FB_W) win->r.x = FB_W - win->r.w;
        if (win->r.y + win->r.h > FB_H) win->r.y = FB_H - win->r.h;
        if (win->r.x < 0) win->r.x = 0;
        if (win->r.y < 0) win->r.y = 0;
    }
}

/* the platform layer (uefi_main.c) calls this after a runtime resolution
   change; the legacy core repaints, the unoui shell reflows its windows */
void uno_screen_changed(void) { if (UI.nwin) reflow(); }

/* ---- action handler ----------------------------------------------------- */
static void on_action(const unoui_action *a)
{
    if (!a->changed) return;
    switch (a->id) {
    case ID_THEME:
        if (a->value >= 0 && a->value < NTHEMES)
            unoui_ui_theme(&UI, kThemes[a->value].theme);
        break;
    case ID_RES:
        if (a->value >= 0 && a->value < g_res_n) {
            uno_pc64_res_set(a->value);
            reflow();
        }
        break;
    case ID_SAVE: case ID_MENU:                /* Save button, or File>Save */
        if (a->id == ID_SAVE) editor_save();
        else if (a->value == 2) editor_save();  /* File menu item 2 = Save */
        else if (a->value == 1) editor_open();  /* Open */
        else if (a->value == 0) { unoui_text_set(&g_body_t, ""); strcpy(g_status, "new"); } /* New */
        break;
    case ID_OPEN: editor_open(); break;
    case ID_NEWF: unoui_text_set(&g_body_t, ""); strcpy(g_status, "new"); break;
    case ID_ABOUT: strcpy(g_status, "UnoDOS pc64 unoui shell v0.3"); break;
    default: break;
    }
}

/* ---- UEFI input -> unoui_event ----------------------------------------- */
static void feed(const unoui_event *ev) { unoui_action a = unoui_handle(&UI, ev); on_action(&a); }

static void pump_input(void)
{
    static int lastx = -1, lasty = -1, lastb = 0;
    int mx, my, mb, scan, uni, ctrl;
    unoui_event ev;

    /* pointer */
    uno_pc64_mouse(&mx, &my, &mb);
    if (mx != lastx || my != lasty) {
        memset(&ev, 0, sizeof ev); ev.kind = UI_EV_MOUSE_MOVE; ev.x = mx; ev.y = my;
        feed(&ev); lastx = mx; lasty = my;
    }
    if (mb != lastb) {
        memset(&ev, 0, sizeof ev);
        ev.kind = mb ? UI_EV_MOUSE_DOWN : UI_EV_MOUSE_UP; ev.x = mx; ev.y = my;
        feed(&ev); lastb = mb;
    }

    /* keyboard */
    while (uno_pc64_next_key(&scan, &uni, &ctrl)) {
        int mods = ctrl ? UI_MOD_CTRL : 0;
        int vk = 0;
        switch (scan) {
        case 0x01: vk = UI_KEY_UP; break;    case 0x02: vk = UI_KEY_DOWN; break;
        case 0x03: vk = UI_KEY_RIGHT; break; case 0x04: vk = UI_KEY_LEFT; break;
        case 0x05: vk = UI_KEY_HOME; break;  case 0x06: vk = UI_KEY_END; break;
        case 0x09: vk = UI_KEY_PGUP; break;  case 0x0A: vk = UI_KEY_PGDN; break;
        case 0x08: vk = UI_KEY_DELETE; break;case 0x17: vk = UI_KEY_ESC; break;
        }
        if (!vk) {
            if (uni == 0x0D || uni == 0x0A) vk = UI_KEY_ENTER;
            else if (uni == 0x08) vk = UI_KEY_BACKSPACE;
            else if (uni == 0x09) vk = UI_KEY_TAB;
        }
        memset(&ev, 0, sizeof ev);
        if (vk) { ev.kind = UI_EV_KEY; ev.key = vk; ev.mods = mods; feed(&ev); }
        else if (uni >= 32 && uni < 127) { ev.kind = UI_EV_CHAR; ev.ch = uni; feed(&ev); }
    }
}

int main(void)
{
    unoui_event tick;
    uno_pc64_init();
    unoui_ui_init(&UI, &theme_unodos, FB_W, FB_H);
    build_windows();

    memset(&tick, 0, sizeof tick); tick.kind = UI_EV_TICK;
    for (;;) {
        uno_pc64_poll();            /* pump UEFI input into the raw streams */
        pump_input();               /* -> unoui events + action handling */
        feed(&tick);                /* caret blink */
        unoui_render_ui(&UI);
        uno_pc64_present();
    }
    return 0;
}

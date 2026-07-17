/* ===========================================================================
 * UnoDOS/pc64 - the unoui SHELL (built with -DUNO_UUI in place of unodos.c).
 *
 * Makes the cross-platform unoui toolkit the whole UI: a themed desktop +
 * window manager + retained-mode widgets. A persistent LAUNCHER opens app
 * windows on demand (raise if already open, Ctrl-W to close). unoui owns the
 * windows, focus, dragging, menus, widgets and rendering into `fb`;
 * uno_pc64_present() scales `fb` to the panel.
 *
 * The frame is only redrawn/presented when something changed (input, or the
 * caret blink) - idle frames touch no VRAM, so the desktop is rock-steady and
 * a drag only rewrites the moving window's rows (no full-screen churn).
 * ======================================================================== */
#include "unoui.h"
#include "unoui_theme.h"
#include "mac_compat.h"      /* FB_W/FB_H + uno_pc64_* + FSOpen/... */
#include "pc64_uui_apps.h"   /* the legacy-app bridge (paint, tracker, music) */
#include "pc64_games.h"      /* native unoui games (Dostris, ...) */
#include "pc64_browser.h"    /* the web browser (native windowed canvas) */
#include "pc64_icons.h"      /* per-app icon artwork */
#include "pc64_font.h"       /* TrueType text engine (system font) */
#include "unosound.h"        /* UnoSound live sequencer (game/app audio) */
#include <string.h>

/* ---- themes (dropdown + live re-skin) ---------------------------------- */
static const struct { const char *name; const struct unoui_theme *theme; } kThemes[] = {
    { "Aurora Light", &theme_aurora_light }, { "Aurora Dark", &theme_aurora_dark },
    { "UnoDOS",    &theme_unodos  }, { "Mac OS 7",    &theme_macos7 },
    { "Mac Plus",  &theme_macplus }, { "Windows 3.1", &theme_win31  },
    { "Amiga",     &theme_amiga   }, { "C64",         &theme_c64    },
    { "Apple II",  &theme_apple2  }, { "NeXTSTEP",    &theme_next   }
};
#define NTHEMES ((int)(sizeof kThemes / sizeof kThemes[0]))
static const char *kThemeNames[NTHEMES];

/* ---- apps --------------------------------------------------------------- *
 * The first NNATIVE are unoui-native (built from widgets). The rest are the
 * migrated legacy apps (games / paint / tracker / music), each hosted in a
 * canvas via the pc64_uui_apps bridge; app index a>=NNATIVE maps to legacy
 * index a-NNATIVE. */
enum { APP_CTRL, APP_EDIT, APP_FILES, APP_SYS, APP_CLOCK, APP_DEMO, NNATIVE };
#define NEXTRA 2                          /* extra native apps beyond the bridge */
#define EX_RUNNER  (NNATIVE + UNOAPP_COUNT)       /* Runner3D: shell app index    */
#define EX_BROWSER (NNATIVE + UNOAPP_COUNT + 1)   /* Browser: shell app index     */
#define NAPPS  (NNATIVE + UNOAPP_COUNT + NEXTRA)
#define APP_TBAR 18                       /* legacy apps' own title-bar height */
static const char *kAppNames[NNATIVE] =
    { "Control Panel", "Editor", "Files", "System", "Clock", "Canvas" };

#define TASKH 26                          /* taskbar height, px */

static unoui_ui     UI;
static unoui_window g_launch;             /* app menu, opened by the Start button */
static unoui_window g_desk;               /* bare/bottom: the desktop-icon layer */
static unoui_window g_task;               /* bare/top: the taskbar */
static unoui_window g_win[NAPPS];
static int          g_built[NAPPS], g_open[NAPPS];
static int          g_dirty = 1;

/* one canvas per legacy app; ctx carries its legacy index */
static unoui_canvas g_lcanvas[UNOAPP_COUNT];
static int          g_lidx[UNOAPP_COUNT];

static const char *kNativeShort[NNATIVE] =
    { "Control", "Editor", "Files", "System", "Clock", "Canvas" };
static const char *app_name(int a)
{ return a == EX_RUNNER ? "Runner3D" : a == EX_BROWSER ? "Browser"
       : a < NNATIVE ? kAppNames[a] : unoapp_name(a - NNATIVE); }
static const char *app_short(int a)
{ return a == EX_RUNNER ? "Runner" : a == EX_BROWSER ? "Browser"
       : a < NNATIVE ? kNativeShort[a] : unoapp_name(a - NNATIVE); }

/* widget ids */
enum { ID_THEME = 1, ID_RES, ID_DARK, ID_WRAP, ID_VOL, ID_SCALE, ID_ABOUT,
       ID_MENU, ID_BODY, ID_NAME, ID_SAVE, ID_OPEN, ID_NEWF, ID_FILES, ID_FMT,
       ID_FULL, ID_DATE, ID_TIME, ID_SETDT, ID_FONT, ID_CAL, ID_EFONT,
       ID_START = 90, ID_SHUTDOWN = 91, ID_RESTART = 92,
       ID_LAUNCH0 = 100,                  /* desktop icons + launcher: +app     */
       ID_TASK0   = 200 };                /* taskbar window buttons: +app       */

/* editor + status buffers */
static char g_body[1024], g_name[32], g_status[48] = "ready";
static unoui_text g_body_t, g_name_t;
static char g_res_str[12][14]; static const char *g_res_items[12]; static int g_res_n;
static const char *g_files[] = { "README.TXT", "NOTES.TXT", "SONG.TRK" };
static const char *g_fmts[]  = { "Plain", "Markdown", "UnoDOS" };
static char g_clock[40] = "Uptime 0 s";

/* ---- RAM-disk File Manager glue ---------------------------------------- */
static void c2p(const char *s, unsigned char *p)
{ int n = (int)strlen(s); if (n > 31) n = 31; p[0] = (unsigned char)n;
  { int i; for (i = 0; i < n; i++) p[i + 1] = (unsigned char)s[i]; } }

static void editor_save(void)
{
    unsigned char pn[34]; short ref; long n = g_body_t.len;
    c2p(g_name[0] ? g_name : "NOTES.TXT", pn);
    Create(pn, 0, 0, 0);
    if (FSOpen(pn, 0, &ref) == noErr) { FSWrite(ref, &n, g_body); FSClose(ref);
        strcpy(g_status, "saved"); } else strcpy(g_status, "save failed");
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

static void build_res_items(void)
{
    int i, n = uno_pc64_res_count(); if (n > 12) n = 12;
    for (i = 0; i < n; i++) {
        short w, h, z; Boolean act; char *o = g_res_str[i]; int j = 0;
        uno_pc64_res_get(i, &w, &h, &z, &act);
        { int v = w, k = 0; char t[8]; if (!v) t[k++]='0'; while (v){t[k++]='0'+v%10;v/=10;}
          while (k) o[j++] = t[--k]; }
        o[j++] = 'x';
        { int v = h, k = 0; char t[8]; if (!v) t[k++]='0'; while (v){t[k++]='0'+v%10;v/=10;}
          while (k) o[j++] = t[--k]; }
        o[j] = 0; g_res_items[i] = o;
    }
    g_res_n = n;
}

/* ---- menus ------------------------------------------------------------- */
static const char *m_file[] = { "New", "Open", "Save" };
static const char *m_edit[] = { "Cut", "Copy", "Paste" };
static const unoui_menu g_menus[] = { { "File", m_file, 3 }, { "Edit", m_edit, 3 } };

/* ---- app window builders (sized so nothing overflows: usable content
 *      height is win.h - title_h(18) - 2*pad(10) - frame ~= win.h - 40) ---- */
static unoui_widget *g_sp_y, *g_sp_mo, *g_sp_d, *g_sp_h, *g_sp_mi;

static const char *g_font_items[6];
static int         g_font_n;
static void build_font_items(void)
{
    int i, nf = uno_font_count();
    g_font_items[0] = "System (mono)";
    for (i = 0; i < nf && i < 5; i++) g_font_items[1 + i] = uno_font_name(i);
    g_font_n = 1 + (nf > 5 ? 5 : nf);
}

static void build_ctrl(unoui_window *w)
{
    unoui_widget *x; int i;
    int yy = 2026, mo = 1, dd = 1, hh = 0, mi = 0;
    for (i = 0; i < NTHEMES; i++) kThemeNames[i] = kThemes[i].name;
    build_res_items();
    build_font_items();
    unoui_window_init(w, "Control Panel", 150, 24, 270, 272);
    unoui_add_label(w, 8, 8, "Theme:");
    x = unoui_add_dropdown(w, 74, 4, 170, kThemeNames, NTHEMES, 0); x->id = ID_THEME;
    unoui_add_label(w, 8, 34, "Resolution:");
    x = unoui_add_dropdown(w, 100, 30, 144, g_res_items, g_res_n, 0); x->id = ID_RES;
    unoui_add_label(w, 8, 60, "Font:");
    x = unoui_add_dropdown(w, 74, 56, 170, g_font_items, g_font_n, uno_font_active()+1); x->id = ID_FONT;
    unoui_add_check(w, 8, 86, "Dark mode", 0);   w->w[w->nw-1].id = ID_DARK;
    unoui_add_check(w, 130, 86, "Word wrap", 1); w->w[w->nw-1].id = ID_WRAP;
    unoui_add_label(w, 8, 112, "Volume");
    x = unoui_add_slider(w, 66, 108, 178, 0, 100, 70); x->id = ID_VOL;
    unoui_add_label(w, 8, 136, "UI scale");
    x = unoui_add_spinner(w, 72, 132, 70, 1, 4, 1); x->id = ID_SCALE;
    x = unoui_add_button(w, 150, 132, 94, "About", 0); x->id = ID_ABOUT;
    /* --- date & time (firmware RTC) --- */
    unoui_add_sep(w, 8, 158, 236);
    unoui_add_label(w, 8, 166, "Set date & time:");
    uno_pc64_time(&yy, &mo, &dd, &hh, &mi, 0);
    g_sp_y  = unoui_add_spinner(w, 8,   182, 58, 2000, 2099, yy);
    g_sp_mo = unoui_add_spinner(w, 70,  182, 44, 1, 12, mo);
    g_sp_d  = unoui_add_spinner(w, 118, 182, 44, 1, 31, dd);
    g_sp_h  = unoui_add_spinner(w, 166, 182, 40, 0, 23, hh);
    g_sp_mi = unoui_add_spinner(w, 210, 182, 40, 0, 59, mi);
    x = unoui_add_button(w, 8, 208, 100, "Apply Clock", 0); x->id = ID_SETDT;
    x = unoui_add_button(w, 116, 208, 128, "Pick date...", 0); x->id = ID_CAL;
}
static void build_edit(unoui_window *w)
{
    unoui_widget *x;
    build_font_items();
    unoui_window_init(w, "Editor", 150, 60, 300, 314);
    unoui_text_init(&g_body_t, g_body, sizeof g_body, 1);
    unoui_text_set(&g_body_t, "unoui is the whole UnoDOS UI now.\n"
                              "Open apps from the launcher; drag,\n"
                              "click or Tab/arrows/Enter. Edit me,\n"
                              "then Save (Ctrl-W closes a window).");
    unoui_text_init(&g_name_t, g_name, sizeof g_name, 0);
    unoui_text_set(&g_name_t, "NOTES.TXT");
    x = unoui_add_menubar(w, g_menus, 2); x->id = ID_MENU;
    unoui_add_label(w, 4, 30, "Body:");
    x = unoui_add_textarea(w, 4, 42, 262, 120, &g_body_t); x->id = ID_BODY;
    unoui_add_vscroll(w, 268, 42, 120, 0, 100);
    unoui_add_label(w, 4, 172, "File:");
    x = unoui_add_edit(w, 40, 168, 130, &g_name_t); x->id = ID_NAME;
    unoui_add_label(w, 178, 172, "Fmt:");
    x = unoui_add_dropdown(w, 210, 168, 80, g_fmts, 3, 2); x->id = ID_FMT;
    x = unoui_add_button(w, 4,  198, 80, "Save", UI_F_DEFAULT); x->id = ID_SAVE;
    x = unoui_add_button(w, 90, 198, 80, "Open", 0); x->id = ID_OPEN;
    x = unoui_add_button(w, 176, 198, 80, "New", 0); x->id = ID_NEWF;
    unoui_add_label(w, 4, 228, "Doc font:");
    x = unoui_add_dropdown(w, 78, 224, 178, g_font_items, g_font_n,
                           w->font_slot == UI_FONT_INHERIT ? 0 : w->font_slot + 1); x->id = ID_EFONT;
    unoui_add_label(w, 4, 256, g_status);
}
static void build_files(unoui_window *w)
{
    unoui_widget *x;
    unoui_window_init(w, "Files", 8, 240, 180, 168);
    unoui_add_label(w, 6, 6, "RAM disk:");
    x = unoui_add_list(w, 6, 22, 156, 100, g_files, 3, 0); x->id = ID_FILES;
}
/* tiny no-libc string builders for the diagnostics line */
static char *ap_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *ap_int(char *p, int v) { char t[12]; int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; } if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; } while (n) *p++ = t[--n]; return p; }
static char *ap_hex(char *p, int v) { const char *h = "0123456789abcdef";
    *p++ = '0'; *p++ = 'x'; *p++ = h[(v >> 4) & 0xF]; *p++ = h[v & 0xF]; return p; }

static char g_tp1[64], g_tp2[64];
static void build_tpstat(void)
{
    int nb = 0, nc = 0, pr = 0, ad = 0, pa = 0; char *p;
    uno_i2c_hid_status(&nb, &nc, &pr, &ad, &pa);
    p = ap_str(g_tp1, "Trackpad I2C: ");
    p = ap_int(p, nc); p = ap_str(p, " DW ctrl / ");
    p = ap_int(p, nb); p = ap_str(p, " bars"); *p = 0;
    p = ap_str(g_tp2, "  HID device: ");
    if (pr) { p = ap_str(p, "UP  addr "); p = ap_hex(p, ad);
              p = ap_str(p, pa ? "  parsed" : "  UNPARSED"); }
    else    { p = ap_str(p, nc ? "not found on ctrl" : "no controller (ACPI-only?)"); }
    *p = 0;
}

static void build_sys(unoui_window *w)
{
    build_tpstat();
    unoui_window_init(w, "System", 400, 240, 320, 140);
    unoui_add_label(w, 8, 6,  "UnoDOS / pc64  -  unoui shell");
    unoui_add_label(w, 8, 24, "x86-64 UEFI  -  bare metal");
    unoui_add_label(w, 8, 42, "8 themes  -  live re-skin");
    unoui_add_sep  (w, 8, 60, 300);
    unoui_add_label(w, 8, 68, g_tp1);
    unoui_add_label(w, 8, 84, g_tp2);
}
static void build_clock(unoui_window *w)
{
    unoui_window_init(w, "Clock", 420, 30, 200, 80);
    unoui_add_label(w, 10, 10, g_clock);
    unoui_add_label(w, 10, 30, "(uptime since boot)");
}

/* ---- Canvas demo: proves UI_CANVAS (app-drawn region) + unoui_fullscreen -- */
static void canvas_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    int i; (void)w; (void)ctx;
    for (i = 0; i < 8; i++) {                       /* colour bars */
        fb_px c = FB_RGB((i & 1) ? 255 : 0, (i & 2) ? 255 : 0, (i & 4) ? 255 : 0);
        fb_fill_rect(r.x + r.w * i / 8, r.y, r.w / 8 + 1, r.h, c);
    }
    fb_frame_rect(r.x, r.y, r.w, r.h, FB_RGB(255, 255, 255));
    /* caption on a dark backing strip so it reads over the bars and fits */
    fb_fill_rect(r.x + 1, r.y + 1, r.w - 2, 30, FB_RGB(0, 0, 0));
    fb_text(r.x + 6, r.y + 5,  "UI_CANVAS: the app owns",
            FB_RGB(255, 255, 255), FB_RGB(0, 0, 0));
    fb_text(r.x + 6, r.y + 17, "these pixels.",
            FB_RGB(255, 255, 255), FB_RGB(0, 0, 0));
}
static int canvas_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; (void)w; (void)ctx;
    if (e->kind == UI_EV_KEY && e->key == UI_KEY_ESC) {
        unoui_fullscreen(&UI, 0); g_dirty = 1; return 1;    /* exit fullscreen */
    }
    return 0;
}
static unoui_canvas g_canvas = { canvas_draw, canvas_event, 0 };

static void build_demo(unoui_window *w)
{
    unoui_widget *x;
    unoui_window_init(w, "Canvas", 200, 120, 270, 210);
    unoui_add_label(w, 6, 6, "App-drawn canvas (UI_CANVAS):");
    unoui_add_canvas(w, 6, 20, 256, 120, &g_canvas);
    x = unoui_add_button(w, 6, 150, 120, "Fullscreen", UI_F_DEFAULT); x->id = ID_FULL;
}

static void (*const g_build[NNATIVE])(unoui_window *) =
    { build_ctrl, build_edit, build_files, build_sys, build_clock, build_demo };

/* ---- window management -------------------------------------------------- */
static int g_launch_open;
static int g_menu_scroll, g_menu_hot = -1, g_scroll_tmr;   /* Start-menu scroll */
static unoui_window g_cal;                 /* calendar date-picker popup */
static int g_cal_open, g_cal_y = 2026, g_cal_mo = 1, g_cal_sel = 1;
static unoui_rect g_cal_rect;

static void raise_win(unoui_window *win) { unoui_bring_to_front(&UI, win); }

static void remove_win(unoui_window *win)
{
    int i, j;
    for (i = 0; i < UI.nwin; i++) if (UI.win[i] == win) break;
    if (i >= UI.nwin) return;
    for (j = i; j < UI.nwin - 1; j++) UI.win[j] = UI.win[j + 1];
    UI.nwin--;
    if (UI.focus_win >= UI.nwin) UI.focus_win = UI.nwin - 1;
    UI.focus_wi = -1;
}

/* the taskbar background: a bare window draws no chrome, so a non-interactive
 * canvas paints the bar face + top highlight under the buttons. */
/* forward decls (taskbar events fire these, defined below) */
static void toggle_launcher(void);
static void open_app(int a);
static void fmt_clock(int uptime_secs);

/* ---- taskbar: a single canvas we draw + hit-test by hand for full control -- */
#define TB_START_X 4
#define TB_START_W 60
#define TB_CHIP_X  70
#define TB_CHIP_W  108
#define TB_CHIP_GAP 112

/* app index of the focused window, or -1 (used to highlight its taskbar chip) */
static int focused_app(void)
{
    int i;
    if (UI.focus_win < 0 || UI.focus_win >= UI.nwin) return -1;
    for (i = 0; i < NAPPS; i++) if (&g_win[i] == UI.win[UI.focus_win]) return i;
    return -1;
}

/* a raised (or pressed) 3D panel - the shared taskbar-chip / Start look */
static void tb_panel(int x, int y, int w, int h, fb_px face, int pressed)
{
    const unoui_theme *t = UI.theme;
    fb_fill_rect(x, y, w, h, face);
    fb_frame_rect(x, y, w, h, t->pal.dark);          /* crisp outer edge */
    if (pressed) {
        fb_hline(x + 1, y + 1, w - 2, t->pal.shadow);
        fb_vline(x + 1, y + 1, h - 2, t->pal.shadow);
    } else {
        fb_hline(x + 1, y + 1, w - 2, t->pal.light);
        fb_vline(x + 1, y + 1, h - 2, t->pal.light);
        fb_hline(x + 1, y + h - 2, w - 2, t->pal.shadow);
        fb_vline(x + w - 2, y + 1, h - 2, t->pal.shadow);
    }
}

static void taskbar_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = UI.theme;
    int modern = t->m.radius > 0;
    int i, x, act = focused_app(), by = r.y + (modern ? 5 : 3), bh = TASKH - (modern ? 10 : 6);
    int cr = bh/2 > 8 ? 8 : bh/2;                 /* chip corner radius */
    (void)w; (void)ctx;
    if (modern) {                                 /* Aurora: a frosted bar + hairline */
        fb_blend_rect(r.x, r.y, r.w, r.h, t->pal.win_bg, 236);
        fb_blend_rect(r.x, r.y, r.w, 1, t->pal.text_dim, 45);
    } else {
        fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.face);
        fb_hline(r.x, r.y, r.w, t->pal.light);
    }
    /* Start button - accent-coloured, 4-square emblem + label */
    { int bx = r.x + TB_START_X;
      fb_px q[4] = { FB_RGB(255,255,255), FB_RGB(210,225,255),
                     FB_RGB(225,235,255), FB_RGB(255,255,255) };
      int gx = bx + 7, gy = by + (bh - 11) / 2, u = 5;
      if (modern) fb_round_rect(bx, by, TB_START_W, bh, cr, t->pal.accent);
      else        tb_panel(bx, by, TB_START_W, bh, t->pal.accent, 0);
      fb_fill_rect(gx, gy, u, u, q[0]);       fb_fill_rect(gx + u + 1, gy, u, u, q[1]);
      fb_fill_rect(gx, gy + u + 1, u, u, q[2]); fb_fill_rect(gx + u + 1, gy + u + 1, u, u, q[3]);
      fb_text(bx + 22, by + (bh - 8) / 2, "Start", t->pal.accent_text, -1); }
    /* one chip per open window: mini icon + name, highlighted if it's active */
    x = r.x + TB_CHIP_X;
    for (i = 0; i < NAPPS; i++) {
        int d = (i == act) ? 1 : 0;
        unoui_rect eb;
        if (!g_open[i]) continue;
        if (modern) {
            if (d) { fb_round_rect_a(x, by, TB_CHIP_W, bh, cr, t->pal.accent, 48, FB_CORNER_ALL);
                     fb_fill_rect(x + 8, by + bh - 2, TB_CHIP_W - 16, 2, t->pal.accent); }
            else     fb_round_rect_a(x, by, TB_CHIP_W, bh, cr, t->pal.text, 18, FB_CORNER_ALL);
        } else tb_panel(x, by, TB_CHIP_W, bh, t->pal.face, d);
        { int dd = modern ? 0 : d;
          eb.x = x + 4 + dd; eb.y = by + (bh - 16) / 2 + dd; eb.w = 16; eb.h = 16;
          pc64_icon_emblem(i, eb);
          fb_set_clip(x + 22, by, TB_CHIP_W - 24, bh);        /* keep the name in the chip */
          fb_text(x + 24 + dd, by + (bh - 8) / 2 + dd, app_short(i), t->pal.text, -1);
          fb_set_clip(r.x, r.y, r.w, r.h); }                  /* back to the bar */
        x += TB_CHIP_GAP;
    }
    /* system tray: a clock chip, right-aligned */
    { int cw = fb_text_w(g_clock) + 16, cxx = r.x + r.w - cw - 6;
      if (modern) fb_round_rect_a(cxx, by, cw, bh, cr, t->pal.text, 16, FB_CORNER_ALL);
      else        tb_panel(cxx, by, cw, bh, t->pal.field_bg, 1);
      fb_text(cxx + 8, r.y + (r.h - 8) / 2, g_clock, modern ? t->pal.text : t->pal.field_text, -1); }
}

static int taskbar_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    int px, i, x;
    (void)w; (void)ctx;
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    px = e->x - g_task.r.x;
    if (px >= TB_START_X && px < TB_START_X + TB_START_W) { toggle_launcher(); return 1; }
    x = TB_CHIP_X;
    for (i = 0; i < NAPPS; i++) {
        if (!g_open[i]) continue;
        if (px >= x && px < x + TB_CHIP_W) { open_app(i); return 1; }
        x += TB_CHIP_GAP;
    }
    return 0;
}
static unoui_canvas g_taskcv = { taskbar_draw, taskbar_event, 0 };

/* the taskbar is one canvas; it reads g_open live, so "rebuild" is just a
 * redraw request (kept as a name the open/close paths already call). */
static void rebuild_taskbar(void) { g_dirty = 1; }

static void build_taskbar(void)
{
    unoui_window_init(&g_task, "", 0, FB_H - TASKH, FB_W, TASKH);
    g_task.flags = UI_WIN_BARE | UI_WIN_TOP;
    unoui_add_canvas(&g_task, 0, 0, FB_W, TASKH, &g_taskcv);
}

static void build_desktop(void)
{
    int i, percol;
    unoui_window_init(&g_desk, "", 0, 0, FB_W, FB_H - TASKH);
    g_desk.flags = UI_WIN_BARE | UI_WIN_BOTTOM;
    percol = (FB_H - TASKH - 20) / 52; if (percol < 1) percol = 1;   /* grid */
    for (i = 0; i < NAPPS; i++) {
        int col = i / percol, row = i % percol;
        unoui_widget *ic = unoui_add_icon(&g_desk, 16 + col * 104, 14 + row * 52,
                                          app_short(i));
        ic->r.w = 84; ic->r.h = 48;         /* room for emblem + label         */
        ic->icon = i;                       /* -> pc64_icon_art draws its art  */
        ic->id  = ID_LAUNCH0 + i;
    }
}

/* pull a window fully into the work area (screen minus the taskbar) */
static void clamp_to_workarea(unoui_window *w)
{
    if (w->r.x + w->r.w > FB_W)         w->r.x = FB_W - w->r.w;
    if (w->r.y + w->r.h > FB_H - TASKH) w->r.y = FB_H - TASKH - w->r.h;
    if (w->r.x < 0) w->r.x = 0;
    if (w->r.y < 0) w->r.y = 0;
}

/* ---- legacy apps hosted in a canvas ------------------------------------- */
static void lcanvas_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{ (void)w; unoapp_paint(*(int *)ctx, r); }

static int lcanvas_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; (void)w;
    if (UI.full && e->kind == UI_EV_KEY && e->key == UI_KEY_ESC) {
        unoui_fullscreen(&UI, 0); g_dirty = 1; return 1;   /* leave fullscreen */
    }
    if (unoapp_input(*(int *)ctx, e)) { g_dirty = 1; return 1; }
    return 0;
}

/* the native GAME index for shell app `a`, or -1 (the rest use the bridge) */
static int app_game(int a)
{
    int li = a - NNATIVE, g = -1;
    if (a < NNATIVE) return -1;
    if (a == EX_RUNNER)             g = GAME_RUNNER;        /* extra native app */
    else if (li >= 0 && li < 3)     g = li;                 /* Dostris/Pacman/Outlast */
    return (g >= 0 && pc64_game_canvas(g)) ? g : -1;
}
/* a mac_compat-bridge app (Music/Tracker/Paint/Network - not a native game/browser) */
static int app_is_bridge(int a)
{ int li = a - NNATIVE; return a >= NNATIVE && li >= 0 && li < UNOAPP_COUNT && app_game(a) < 0; }

static void build_legacy(int a)
{
    int li = a - NNATIVE, aw, ah, g = app_game(a);
    const unoui_metrics *m = &UI.theme->m;
    unoui_canvas *cv;
    if (g >= 0) {                      /* native game: one canvas that scales */
        aw = 320; ah = 240;
        unoui_window_init(&g_win[a], app_name(a), 30, 16,
                          aw + 2*m->frame_w + 2*m->pad, ah + m->title_h + 2*m->pad + m->frame_w);
        unoui_widget_fill(unoui_add_canvas(&g_win[a], 0, 0, aw, ah, pc64_game_canvas(g)));
        g_win[a].flags |= UI_WIN_RESIZE;    /* the game canvas scales to the rect */
        return;
    }
    if (a == EX_BROWSER) {             /* native windowed browser canvas */
        aw = 440; ah = 300;
        unoui_window_init(&g_win[a], app_name(a), 24, 14,
                          aw + 2*m->frame_w + 2*m->pad, ah + m->title_h + 2*m->pad + m->frame_w);
        unoui_widget_fill(unoui_add_canvas(&g_win[a], 0, 0, aw, ah, pc64_browser_canvas()));
        g_win[a].flags |= UI_WIN_RESIZE;    /* text reflows to the new width */
        return;
    }
    cv = &g_lcanvas[li];
    unoapp_size(li, &aw, &ah);
    if (aw < 140) aw = 140;
    if (ah < 100) ah = 100;
    g_lidx[li] = li;
    cv->draw = lcanvas_draw; cv->event = lcanvas_event; cv->ctx = &g_lidx[li];
    /* size the window so its content area == the app's drawable area (the app
     * minus its own title bar, which unoui draws instead). */
    unoui_window_init(&g_win[a], app_name(a), 40, 20,
                      aw + 2 * m->frame_w + 2 * m->pad,
                      (ah - APP_TBAR) + m->title_h + 2 * m->pad + m->frame_w);
    unoui_widget_fill(unoui_add_canvas(&g_win[a], 0, 0, aw, ah - APP_TBAR, cv));
    g_win[a].flags |= UI_WIN_RESIZE;
}

static void open_app(int a)
{
    if (a < 0 || a >= NAPPS) return;
    if (!g_built[a]) {
        if (a < NNATIVE) g_build[a](&g_win[a]);
        else             build_legacy(a);
        g_built[a] = 1;
    }
    if (!g_open[a]) {
        int g = app_game(a);
        clamp_to_workarea(&g_win[a]);   /* designed pos, but never off-screen */
        unoui_ui_add(&UI, &g_win[a]); g_open[a] = 1;
        if (g >= 0)                 pc64_game_open(g);           /* native game   */
        else if (a == EX_BROWSER)   pc64_browser_open();         /* browser       */
        else if (app_is_bridge(a))  unoapp_open(a - NNATIVE);    /* bridge app    */
        rebuild_taskbar();
    } else raise_win(&g_win[a]);
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }  /* Start-menu closes */
    /* focus the opened window + its canvas (closing the launcher above moved
     * focus to the taskbar, so do this last). */
    { int fi; for (fi = 0; fi < UI.nwin; fi++) if (UI.win[fi] == &g_win[a]) { UI.focus_win = fi; break; } }
    if (a >= NNATIVE) UI.focus_wi = 0;   /* focus the app's canvas for keyboard */
    /* native games scale to any rect, so they can fill the screen (Esc returns).
     * Bridge apps + the browser stay windowed (they draw at a fixed size). */
    if (app_game(a) >= 0) unoui_fullscreen(&UI, &g_win[a]);
    g_dirty = 1;
}

/* Start button: toggle the app-menu launcher window */
static void toggle_launcher(void)
{
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }
    else { g_menu_scroll = 0; g_menu_hot = 0;
           clamp_to_workarea(&g_launch); unoui_ui_add(&UI, &g_launch);
           UI.focus_wi = 0;                    /* focus the menu canvas for keys */
           g_launch_open = 1; }
    g_dirty = 1;
}

/* F2 / Ctrl-Tab: raise the next OPEN app window (skips desktop + taskbar) */
static void cycle_window(void)
{
    int i, cur = -1;
    if (UI.focus_win >= 0 && UI.focus_win < UI.nwin)
        for (i = 0; i < NAPPS; i++) if (&g_win[i] == UI.win[UI.focus_win]) { cur = i; break; }
    for (i = 1; i <= NAPPS; i++) {
        int a = (cur + i) % NAPPS;
        if (g_open[a]) { raise_win(&g_win[a]); g_dirty = 1; return; }
    }
}

static void close_focused(void)
{
    int f = UI.focus_win, i;
    unoui_window *win;
    if (f < 0 || f >= UI.nwin) return;
    win = UI.win[f];
    if (win->flags & UI_WIN_BARE) return;         /* never close desktop/taskbar */
    if (win == &g_launch) { remove_win(&g_launch); g_launch_open = 0; g_dirty = 1; return; }
    if (win == &g_cal)    { remove_win(&g_cal);    g_cal_open = 0;    g_dirty = 1; return; }
    for (i = 0; i < NAPPS; i++) if (&g_win[i] == win) {
        int g = app_game(i);
        g_open[i] = 0;
        if (g >= 0)              pc64_game_close(g);        /* native game teardown */
        else if (app_is_bridge(i)) unoapp_close(i - NNATIVE); /* bridge app        */
        break;
    }
    if (UI.full == win) unoui_fullscreen(&UI, 0);   /* closing a fullscreen game */
    remove_win(win);
    rebuild_taskbar();
    g_dirty = 1;
}

/* ---- Start menu: a scrollable canvas (apps + Restart + Shut Down) --------- *
 * Entries beyond the visible window scroll: hovering the bottom edge shows a
 * down chevron and scrolls down; once scrolled, an up chevron appears at the
 * top edge and hovering it scrolls back. */
#define MROW 22
#define MENU_MAXVIS 11

static int  menu_count(void)         { return NAPPS + 2; }   /* apps + restart + shutdown */
static const char *menu_label(int i) { return i < NAPPS ? app_name(i) : (i == NAPPS ? "Restart" : "Shut Down"); }
static int  menu_icon(int i)         { return i < NAPPS ? i : -1; }
static int  menu_vis(void)           { int t = menu_count(); return t < MENU_MAXVIS ? t : MENU_MAXVIS; }

static void menu_activate(int i)
{
    if (i < 0 || i >= menu_count()) return;
    if (i < NAPPS)       open_app(i);            /* also closes the launcher */
    else if (i == NAPPS) uno_pc64_restart();
    else                 uno_pc64_shutdown();
}

static void chevron(int cx, int y, int dir, fb_px c)         /* -1 up, +1 down */
{ int k; for (k = 0; k < 4; k++) { int wd = (dir < 0 ? 2*k+1 : 2*(3-k)+1); fb_hline(cx - wd/2, y + k, wd, c); } }

static void launcher_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = UI.theme; int vis = menu_vis(), total = menu_count(), i;
    (void)w; (void)ctx;
    for (i = 0; i < vis; i++) {
        int idx = g_menu_scroll + i, ry = r.y + i * MROW, hot = (idx == g_menu_hot);
        if (idx >= total) break;
        if (hot) fb_fill_rect(r.x, ry, r.w, MROW, t->pal.accent);
        if (idx == NAPPS) fb_hline(r.x, ry, r.w, t->pal.shadow);     /* power divider */
        if (menu_icon(idx) >= 0) { unoui_rect eb = { r.x + 3, ry + 2, 18, 18 }; pc64_icon_emblem(menu_icon(idx), eb); }
        fb_text(r.x + 26, ry + (MROW - 8) / 2, menu_label(idx),
                hot ? t->pal.accent_text : t->pal.text, -1);
    }
    if (g_menu_scroll > 0)           chevron(r.x + r.w/2, r.y + 1,           -1, t->pal.text);
    if (g_menu_scroll + vis < total) chevron(r.x + r.w/2, r.y + vis*MROW - 6, +1, t->pal.text);
}

/* keep the highlighted entry within the scrolled window */
static void menu_reveal(void)
{
    int vis = menu_vis();
    if (g_menu_hot < g_menu_scroll)           g_menu_scroll = g_menu_hot;
    if (g_menu_hot >= g_menu_scroll + vis)    g_menu_scroll = g_menu_hot - vis + 1;
}

static int launcher_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; int ox, oy, row;
    (void)w; (void)ctx;
    if (e->kind == UI_EV_KEY) {                       /* keyboard navigation */
        int total = menu_count();
        if (g_menu_hot < 0) g_menu_hot = g_menu_scroll;
        if (e->key == UI_KEY_DOWN) { if (g_menu_hot < total - 1) g_menu_hot++; menu_reveal(); g_dirty = 1; return 1; }
        if (e->key == UI_KEY_UP)   { if (g_menu_hot > 0) g_menu_hot--;         menu_reveal(); g_dirty = 1; return 1; }
        if (e->key == UI_KEY_ENTER){ menu_activate(g_menu_hot); return 1; }
        return 0;
    }
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    unoui_content_origin(UI.theme, &g_launch, &ox, &oy);
    row = (e->y - oy) / MROW;
    if (row >= 0 && row < menu_vis()) { menu_activate(g_menu_scroll + row); return 1; }
    return 0;
}
static unoui_canvas g_menu_cv = { launcher_draw, launcher_event, 0 };

/* per-frame while the launcher is open: highlight the hovered row, and scroll
 * when the pointer rests on the top/bottom scroll zone. */
static void launcher_hover(int mx, int my)
{
    int ox, oy, vis = menu_vis(), total = menu_count(), row, oldhot = g_menu_hot;
    unoui_content_origin(UI.theme, &g_launch, &ox, &oy);
    /* only the pointer sitting ON the menu changes the highlight - otherwise
     * leave it (so keyboard selection isn't clobbered every frame). */
    if (mx < g_launch.r.x || mx >= g_launch.r.x + g_launch.r.w) return;
    if (my < oy || my >= oy + vis * MROW) return;
    if (g_menu_scroll > 0 && my < oy + 12) {                        /* up zone */
        if (++g_scroll_tmr >= 6) { g_scroll_tmr = 0; g_menu_scroll--; g_dirty = 1; }
        return;
    }
    if (g_menu_scroll + vis < total && my >= oy + vis*MROW - 12) {  /* down zone */
        if (++g_scroll_tmr >= 6) { g_scroll_tmr = 0; g_menu_scroll++; g_dirty = 1; }
        return;
    }
    g_scroll_tmr = 0;
    row = (my - oy) / MROW;
    if (row >= 0 && row < vis) g_menu_hot = g_menu_scroll + row;
    if (g_menu_hot != oldhot) g_dirty = 1;
}

static void build_launcher(void)
{
    const unoui_metrics *m = &UI.theme->m;
    int i, tw = 0, winw, vis = menu_vis();
    for (i = 0; i < menu_count(); i++) { int t = fb_text_w(menu_label(i)); if (t > tw) tw = t; }
    winw = tw + 26 + 14 + 2 * m->frame_w + 2 * m->pad;
    g_menu_scroll = 0; g_menu_hot = -1;
    unoui_window_init(&g_launch, "Programs", 8, 20,
                      winw, m->title_h + m->pad + vis * MROW + m->pad + m->frame_w);
    unoui_add_canvas(&g_launch, 0, 0, winw - 2 * m->frame_w - 2 * m->pad, vis * MROW, &g_menu_cv);
}

/* ---- keep windows on-screen after a resolution change ------------------- */
static void reflow(void)
{
    int i;
    UI.screen_w = FB_W; UI.screen_h = FB_H;
    g_desk.r.w = FB_W; g_desk.r.h = FB_H - TASKH;      /* desktop fills - taskbar */
    g_task.r.y = FB_H - TASKH; g_task.r.w = FB_W;      /* taskbar re-anchored     */
    if (g_task.nw > 0) g_task.w[0].r.w = FB_W;         /* stretch the bar canvas  */
    rebuild_taskbar();
    for (i = 0; i < UI.nwin; i++) {
        unoui_window *win = UI.win[i];
        if (win->flags & UI_WIN_BARE) continue;        /* desk/taskbar done above */
        if (win->r.x + win->r.w > FB_W) win->r.x = FB_W - win->r.w;
        if (win->r.y + win->r.h > FB_H - TASKH) win->r.y = FB_H - TASKH - win->r.h;
        if (win->r.x < 0) win->r.x = 0;
        if (win->r.y < 0) win->r.y = 0;
    }
    g_dirty = 1;
}
void uno_screen_changed(void) { if (UI.nwin) reflow(); }

/* ---- calendar date picker (a popup over the unoui calendar core) --------- */
static void cal_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{ (void)w; (void)ctx; g_cal_rect = r;
  unoui_calendar_draw(UI.theme, r, g_cal_y, g_cal_mo, g_cal_sel); }

static void cal_apply_and_close(void)
{
    if (g_sp_y)  g_sp_y->value  = g_cal_y;
    if (g_sp_mo) g_sp_mo->value = g_cal_mo;
    if (g_sp_d)  g_sp_d->value  = g_cal_sel;
    uno_pc64_set_time(g_cal_y, g_cal_mo, g_cal_sel,
                      g_sp_h ? g_sp_h->value : 0, g_sp_mi ? g_sp_mi->value : 0, 0);
    fmt_clock(0);
    remove_win(&g_cal); g_cal_open = 0; g_dirty = 1;
}

static int cal_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; (void)w; (void)ctx;
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    { int hit = unoui_calendar_hit(g_cal_rect, g_cal_y, g_cal_mo, e->x, e->y);
      if (hit == UI_CAL_PREV) { if (--g_cal_mo < 1) { g_cal_mo = 12; g_cal_y--; } g_dirty = 1; return 1; }
      if (hit == UI_CAL_NEXT) { if (++g_cal_mo > 12) { g_cal_mo = 1;  g_cal_y++; } g_dirty = 1; return 1; }
      if (hit >= 1) { g_cal_sel = hit; cal_apply_and_close(); return 1; } }
    return 0;
}
static unoui_canvas g_cal_cv = { cal_draw, cal_event, 0 };

static void open_calendar(void)
{
    const unoui_metrics *m = &UI.theme->m;
    int cw = 210, chh = 176, yy = 2026, mo = 1, dd = 1, hh = 0, mi = 0;
    if (g_cal_open) { remove_win(&g_cal); g_cal_open = 0; }
    if (g_sp_y) { g_cal_y = g_sp_y->value; g_cal_mo = g_sp_mo->value; g_cal_sel = g_sp_d->value; }
    else { uno_pc64_time(&yy, &mo, &dd, &hh, &mi, 0); g_cal_y = yy; g_cal_mo = mo; g_cal_sel = dd; }
    unoui_window_init(&g_cal, "Pick a date", 180, 56,
                      cw + 2*m->frame_w + 2*m->pad, chh + m->title_h + 2*m->pad + m->frame_w);
    unoui_add_canvas(&g_cal, 0, 0, cw, chh, &g_cal_cv);
    clamp_to_workarea(&g_cal);
    unoui_ui_add(&UI, &g_cal);
    g_cal_open = 1; g_dirty = 1;
}

/* ---- actions ----------------------------------------------------------- */
static void on_action(const unoui_action *a)
{
    if (!a->changed) return;
    g_dirty = 1;
    if (a->kind == UI_ACT_CLOSE) { close_focused(); return; }   /* title-bar close box */
    if (a->id >= ID_TASK0   && a->id < ID_TASK0   + NAPPS) { open_app(a->id - ID_TASK0);   return; }
    if (a->id >= ID_LAUNCH0 && a->id < ID_LAUNCH0 + NAPPS) { open_app(a->id - ID_LAUNCH0); return; }
    switch (a->id) {
    case ID_START:    toggle_launcher(); break;
    case ID_SHUTDOWN: uno_pc64_shutdown(); break;
    case ID_RESTART:  uno_pc64_restart();  break;
    case ID_THEME: if (a->value >= 0 && a->value < NTHEMES) unoui_ui_theme(&UI, kThemes[a->value].theme); break;
    case ID_DARK:  unoui_ui_theme(&UI, a->value ? &theme_aurora_dark : &theme_aurora_light); break;
    case ID_RES:   if (a->value >= 0 && a->value < g_res_n) { uno_pc64_res_set(a->value); reflow(); } break;
    case ID_SAVE:  editor_save(); break;
    case ID_OPEN:  editor_open(); break;
    case ID_NEWF:  unoui_text_set(&g_body_t, ""); strcpy(g_status, "new"); break;
    case ID_MENU:  if (a->value == 2) editor_save(); else if (a->value == 1) editor_open();
                   else if (a->value == 0) { unoui_text_set(&g_body_t, ""); strcpy(g_status, "new"); } break;
    case ID_ABOUT: strcpy(g_status, "unoui shell v0.3"); break;
    case ID_SETDT:
        if (g_sp_y) uno_pc64_set_time(g_sp_y->value, g_sp_mo->value, g_sp_d->value,
                                      g_sp_h->value, g_sp_mi->value, 0);
        fmt_clock(0);
        break;
    case ID_FULL:  unoui_fullscreen(&UI, &g_win[APP_DEMO]); break;   /* go fullscreen */
    case ID_FONT:  uno_font_use(a->value - 1); break;   /* 0 = System bitmap, 1.. = TTF */
    case ID_EFONT: g_win[APP_EDIT].font_slot =          /* Editor per-document font */
                       (a->value <= 0) ? UI_FONT_INHERIT : a->value - 1; break;
    case ID_CAL:   open_calendar(); break;              /* calendar date picker */
    default: break;
    }
}

/* ---- UEFI input -> unoui_event ----------------------------------------- */
static int feed(const unoui_event *ev)
{ unoui_action a = unoui_handle(&UI, ev); on_action(&a); return 1; }

static int pump_input(void)
{
    static int lastx = -1, lasty = -1, lastb = 0;
    int mx, my, mb, scan, uni, ctrl, any = 0;
    unoui_event ev;

    uno_pc64_mouse(&mx, &my, &mb);
    if (mx != lastx || my != lasty) {
        memset(&ev, 0, sizeof ev); ev.kind = UI_EV_MOUSE_MOVE; ev.x = mx; ev.y = my;
        feed(&ev); lastx = mx; lasty = my; any = 1;
    }
    if (mb != lastb) {
        memset(&ev, 0, sizeof ev);
        ev.kind = mb ? UI_EV_MOUSE_DOWN : UI_EV_MOUSE_UP; ev.x = mx; ev.y = my;
        feed(&ev); lastb = mb; any = 1;
    }
    while (uno_pc64_next_key(&scan, &uni, &ctrl)) {
        int mods = ctrl ? UI_MOD_CTRL : 0, vk = 0;
        any = 1;
        if (UI.full && scan == 0x17) {          /* Esc leaves a fullscreen game */
            unoui_fullscreen(&UI, 0); UI.focus_wi = 0; g_dirty = 1; continue;
        }
        if (ctrl && scan == 0x17) { toggle_launcher(); continue; }               /* Ctrl-Esc: Start menu */
        if (ctrl && (uni == 'w' || uni == 'W' || uni == 0x17)) { close_focused(); continue; }  /* Ctrl-W */
        if (scan == 0x0C || (ctrl && uni == 0x09)) { cycle_window(); continue; }  /* F2 / Ctrl-Tab */
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
    return any;
}

/* system-tray clock: the firmware RTC wall time, HH:MM:SS */
static void fmt_clock(int uptime_secs)
{
    int h = 0, mi = 0, s = 0;
    if (uno_pc64_time(0, 0, 0, &h, &mi, &s)) {
        g_clock[0]='0'+(h/10)%10;  g_clock[1]='0'+h%10;  g_clock[2]=':';
        g_clock[3]='0'+(mi/10)%10; g_clock[4]='0'+mi%10; g_clock[5]=':';
        g_clock[6]='0'+(s/10)%10;  g_clock[7]='0'+s%10;  g_clock[8]=0;
    } else {                              /* no RTC (e.g. bare QEMU): uptime */
        int j = 0, v = uptime_secs, k = 0; char t[12];
        strcpy(g_clock, "up ");
        j = 3; if (!v) t[k++]='0'; while (v){t[k++]='0'+v%10; v/=10;}
        while (k) g_clock[j++]=t[--k]; g_clock[j++]='s'; g_clock[j]=0;
    }
}

/* the legacy app index currently in front (fullscreen or focused), or -1 */
static int active_legacy(void)
{
    unoui_window *win = UI.full ? UI.full
                      : (UI.focus_win >= 0 && UI.focus_win < UI.nwin ? UI.win[UI.focus_win] : 0);
    int a;
    if (win) for (a = NNATIVE; a < NAPPS; a++)
        if (&g_win[a] == win && g_open[a]) return a - NNATIVE;
    return -1;
}

int main(void)
{
    unoui_event tick;
    int idle = 0, halfsecs = 0;

    uno_pc64_init();
    unoui_ui_init(&UI, &theme_aurora_light, FB_W, FB_H);   /* modern default look */
    unoui_icon_art = pc64_icon_art;     /* distinct per-app icon artwork */
    unoui_font_push = uno_font_push;    /* per-window font overrides (Editor doc font) */
    unoui_font_pop  = uno_font_pop;
    uno_mac_mouse   = uno_pc64_mac_mouse;   /* live pointer for Paint's drag spin */
    uno_seq_init();                     /* UnoSound: PC-speaker voice */
    uno_seq_backend(uno_pc64_snd_note, uno_pc64_snd_quiet);
    unoapp_setup(&g_dirty);             /* wire the legacy-app KernelApi */
    build_desktop();  unoui_ui_add(&UI, &g_desk);   /* bottom: icon layer  */
    build_taskbar();  unoui_ui_add(&UI, &g_task);   /* top: the taskbar    */
    build_launcher();                                /* opened via Start    */
    fmt_clock(0);                                    /* tray clock ready now */
    uno_font_set_subpixel(1);           /* subpixel AA when a TTF is picked */
    /* default stays the built-in 8x8 bitmap (monospace) - a safe, tested
     * fallback; the Control Panel Font picker opts into a TTF face. */
    open_app(APP_CTRL);                 /* start with Control Panel open */

    memset(&tick, 0, sizeof tick); tick.kind = UI_EV_TICK;
    for (;;) {
        int la;
        uno_pc64_poll();
        if (pump_input()) { g_dirty = 1; idle = 0; }
        else if (++idle >= 30) {        /* ~0.5 s: caret blink + tray clock tick */
            idle = 0; g_dirty = 1;
            fmt_clock(++halfsecs / 2);
        }
        feed(&tick);                    /* advance the caret-blink timebase */
        uno_seq_tick();                 /* UnoSound: advance music/SFX ~60 Hz */
        if (g_launch_open) {            /* Start-menu hover highlight + scroll */
            int mx, my, mb; uno_pc64_mouse(&mx, &my, &mb); launcher_hover(mx, my);
        }
        la = active_legacy();           /* drive the focused game/tool clock */
        { int g = (la >= 0) ? app_game(NNATIVE + la) : -1;
          if (g >= 0) { pc64_game_tick(g); g_dirty = 1; unoapp_focus(-1); }  /* native game */
          else if (la >= 0 && app_is_bridge(NNATIVE + la)) { unoapp_focus(la); unoapp_run_tick(la); }
          else unoapp_focus(-1); }                                            /* browser: no tick */
        if (UI.full) g_dirty = 1;       /* fullscreen apps redraw every frame */
        if (g_dirty) { unoui_render_ui(&UI); uno_pc64_present(); g_dirty = 0; }
        else uno_pc64_delay_ms(16);
    }
    return 0;
}

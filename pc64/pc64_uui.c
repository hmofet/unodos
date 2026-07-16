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
#include <string.h>

/* ---- themes (dropdown + live re-skin) ---------------------------------- */
static const struct { const char *name; const struct unoui_theme *theme; } kThemes[] = {
    { "UnoDOS",    &theme_unodos  }, { "Mac OS 7",    &theme_macos7 },
    { "Mac Plus",  &theme_macplus }, { "Windows 3.1", &theme_win31  },
    { "Amiga",     &theme_amiga   }, { "C64",         &theme_c64    },
    { "Apple II",  &theme_apple2  }, { "NeXTSTEP",    &theme_next   }
};
#define NTHEMES ((int)(sizeof kThemes / sizeof kThemes[0]))
static const char *kThemeNames[NTHEMES];

/* ---- apps -------------------------------------------------------------- */
enum { APP_CTRL, APP_EDIT, APP_FILES, APP_SYS, APP_CLOCK, APP_DEMO, NAPPS };
static const char *kAppNames[NAPPS] =
    { "Control Panel", "Editor", "Files", "System", "Clock", "Canvas" };

#define TASKH 26                          /* taskbar height, px */

static unoui_ui     UI;
static unoui_window g_launch;             /* app menu, opened by the Start button */
static unoui_window g_desk;               /* bare/bottom: the desktop-icon layer */
static unoui_window g_task;               /* bare/top: the taskbar */
static unoui_window g_win[NAPPS];
static int          g_built[NAPPS], g_open[NAPPS];
static int          g_dirty = 1;

/* widget ids */
enum { ID_THEME = 1, ID_RES, ID_DARK, ID_WRAP, ID_VOL, ID_SCALE, ID_ABOUT,
       ID_MENU, ID_BODY, ID_NAME, ID_SAVE, ID_OPEN, ID_NEWF, ID_FILES, ID_FMT,
       ID_FULL, ID_START = 90,
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
static void build_ctrl(unoui_window *w)
{
    unoui_widget *x; int i;
    for (i = 0; i < NTHEMES; i++) kThemeNames[i] = kThemes[i].name;
    build_res_items();
    unoui_window_init(w, "Control Panel", 150, 30, 250, 210);
    unoui_add_label(w, 8, 8, "Theme:");
    x = unoui_add_dropdown(w, 74, 4, 150, kThemeNames, NTHEMES, 0); x->id = ID_THEME;
    unoui_add_label(w, 8, 34, "Resolution:");
    x = unoui_add_dropdown(w, 100, 30, 124, g_res_items, g_res_n, 0); x->id = ID_RES;
    unoui_add_check(w, 8, 60, "Dark mode", 0);   w->w[w->nw-1].id = ID_DARK;
    unoui_add_check(w, 120, 60, "Word wrap", 1); w->w[w->nw-1].id = ID_WRAP;
    unoui_add_label(w, 8, 86, "Volume");
    x = unoui_add_slider(w, 66, 82, 154, 0, 100, 70); x->id = ID_VOL;
    unoui_add_label(w, 8, 110, "UI scale");
    x = unoui_add_spinner(w, 72, 106, 70, 1, 4, 1); x->id = ID_SCALE;
    x = unoui_add_button(w, 8, 138, 100, "About", 0); x->id = ID_ABOUT;
}
static void build_edit(unoui_window *w)
{
    unoui_widget *x;
    unoui_window_init(w, "Editor", 150, 60, 300, 288);
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
    unoui_add_label(w, 4, 228, g_status);
}
static void build_files(unoui_window *w)
{
    unoui_widget *x;
    unoui_window_init(w, "Files", 8, 240, 180, 168);
    unoui_add_label(w, 6, 6, "RAM disk:");
    x = unoui_add_list(w, 6, 22, 156, 100, g_files, 3, 0); x->id = ID_FILES;
}
static void build_sys(unoui_window *w)
{
    unoui_window_init(w, "System", 420, 250, 300, 116);
    unoui_add_label(w, 8, 6,  "UnoDOS / pc64  -  unoui shell");
    unoui_add_label(w, 8, 24, "x86-64 UEFI  -  bare metal");
    unoui_add_label(w, 8, 42, "8 themes  -  live re-skin");
    unoui_add_label(w, 8, 60, "TrackPoint / touchpad + keyboard");
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

static void (*const g_build[NAPPS])(unoui_window *) =
    { build_ctrl, build_edit, build_files, build_sys, build_clock, build_demo };

/* ---- window management -------------------------------------------------- */
static int g_launch_open;

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

/* short desktop-icon / taskbar labels (the window titles are the long names) */
static const char *kShort[NAPPS] =
    { "Control", "Editor", "Files", "System", "Clock", "Canvas" };

/* the taskbar background: a bare window draws no chrome, so a non-interactive
 * canvas paints the bar face + top highlight under the buttons. */
static void taskbg_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = UI.theme; (void)w; (void)ctx;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.face);
    fb_hline(r.x, r.y, r.w, t->pal.light);
    fb_hline(r.x, r.y + 1, r.w, t->pal.light);
}
static unoui_canvas g_taskbg = { taskbg_draw, 0, 0 };

/* rebuild the taskbar widget list: Start + one button per open app + clock.
 * Called on open/close/reflow; the clock label tracks g_clock in place. */
static void rebuild_taskbar(void)
{
    int i, x = 60;
    unoui_widget *b;
    g_task.nw = 0;
    unoui_add_canvas(&g_task, 0, 0, FB_W, TASKH, &g_taskbg);
    b = unoui_add_button(&g_task, 4, 4, 50, "Start", 0); b->id = ID_START;
    for (i = 0; i < NAPPS; i++) {
        if (!g_open[i]) continue;
        b = unoui_add_button(&g_task, x, 4, 100, kShort[i], 0); b->id = ID_TASK0 + i;
        x += 104;
    }
    unoui_add_label(&g_task, FB_W - 118, 8, g_clock);
}

static void build_taskbar(void)
{
    unoui_window_init(&g_task, "", 0, FB_H - TASKH, FB_W, TASKH);
    g_task.flags = UI_WIN_BARE | UI_WIN_TOP;
    rebuild_taskbar();
}

static void build_desktop(void)
{
    int i;
    unoui_window_init(&g_desk, "", 0, 0, FB_W, FB_H - TASKH);
    g_desk.flags = UI_WIN_BARE | UI_WIN_BOTTOM;
    for (i = 0; i < NAPPS; i++) {
        unoui_widget *ic = unoui_add_icon(&g_desk, 16, 14 + i * 52, kShort[i]);
        ic->r.w = 84;                       /* wide enough for the label       */
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

static void open_app(int a)
{
    if (a < 0 || a >= NAPPS) return;
    if (!g_built[a]) { g_build[a](&g_win[a]); g_built[a] = 1; }
    if (!g_open[a]) {
        clamp_to_workarea(&g_win[a]);   /* designed pos, but never off-screen */
        unoui_ui_add(&UI, &g_win[a]); g_open[a] = 1; rebuild_taskbar();
    } else raise_win(&g_win[a]);
    g_dirty = 1;
}

/* Start button: toggle the app-menu launcher window */
static void toggle_launcher(void)
{
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }
    else { unoui_ui_add(&UI, &g_launch); g_launch_open = 1; }
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
    for (i = 0; i < NAPPS; i++) if (&g_win[i] == win) { g_open[i] = 0; break; }
    remove_win(win);
    rebuild_taskbar();
    g_dirty = 1;
}

static void build_launcher(void)
{
    /* Size from theme metrics so buttons fill the content width exactly and
     * the window is wide enough for the longest label - no overflow. Opened by
     * the taskbar Start button. */
    const unoui_metrics *m = &theme_unodos.m;
    int i, tw = 0, bw, winw;
    for (i = 0; i < NAPPS; i++) {                 /* widest label + button pad */
        int w = fb_text_w(kAppNames[i]);
        if (w > tw) tw = w;
    }
    bw   = tw + 32;                                /* label + centred margins  */
    winw = bw + 2 * m->frame_w + 2 * m->pad;
    unoui_window_init(&g_launch, "Programs", 8, 30,
                      winw, m->title_h + m->pad + NAPPS * 28 + m->pad + m->frame_w);
    unoui_widget *x;
    for (i = 0; i < NAPPS; i++) {                 /* flush to content origin   */
        x = unoui_add_button(&g_launch, 0, i * 28, bw, kAppNames[i], 0);
        x->id = ID_LAUNCH0 + i;
    }
}

/* ---- keep windows on-screen after a resolution change ------------------- */
static void reflow(void)
{
    int i;
    UI.screen_w = FB_W; UI.screen_h = FB_H;
    g_desk.r.w = FB_W; g_desk.r.h = FB_H - TASKH;      /* desktop fills - taskbar */
    g_task.r.y = FB_H - TASKH; g_task.r.w = FB_W;      /* taskbar re-anchored     */
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

/* ---- actions ----------------------------------------------------------- */
static void on_action(const unoui_action *a)
{
    if (!a->changed) return;
    g_dirty = 1;
    if (a->id >= ID_TASK0   && a->id < ID_TASK0   + NAPPS) { open_app(a->id - ID_TASK0);   return; }
    if (a->id >= ID_LAUNCH0 && a->id < ID_LAUNCH0 + NAPPS) { open_app(a->id - ID_LAUNCH0); return; }
    switch (a->id) {
    case ID_START: toggle_launcher(); break;
    case ID_THEME: if (a->value >= 0 && a->value < NTHEMES) unoui_ui_theme(&UI, kThemes[a->value].theme); break;
    case ID_RES:   if (a->value >= 0 && a->value < g_res_n) { uno_pc64_res_set(a->value); reflow(); } break;
    case ID_SAVE:  editor_save(); break;
    case ID_OPEN:  editor_open(); break;
    case ID_NEWF:  unoui_text_set(&g_body_t, ""); strcpy(g_status, "new"); break;
    case ID_MENU:  if (a->value == 2) editor_save(); else if (a->value == 1) editor_open();
                   else if (a->value == 0) { unoui_text_set(&g_body_t, ""); strcpy(g_status, "new"); } break;
    case ID_ABOUT: strcpy(g_status, "unoui shell v0.3"); break;
    case ID_FULL:  unoui_fullscreen(&UI, &g_win[APP_DEMO]); break;   /* go fullscreen */
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

static void fmt_uptime(int secs)
{
    int j = 0, v = secs, k = 0; char t[12];
    strcpy(g_clock, "Uptime ");
    j = 7; if (!v) t[k++] = '0'; while (v) { t[k++] = '0' + v % 10; v /= 10; }
    while (k) g_clock[j++] = t[--k];
    g_clock[j++] = ' '; g_clock[j++] = 's'; g_clock[j] = 0;
}

int main(void)
{
    unoui_event tick;
    int idle = 0, halfsecs = 0;

    uno_pc64_init();
    unoui_ui_init(&UI, &theme_unodos, FB_W, FB_H);
    build_desktop();  unoui_ui_add(&UI, &g_desk);   /* bottom: icon layer  */
    build_taskbar();  unoui_ui_add(&UI, &g_task);   /* top: the taskbar    */
    build_launcher();                                /* opened via Start    */
    open_app(APP_CTRL);                 /* start with Control Panel open */

    memset(&tick, 0, sizeof tick); tick.kind = UI_EV_TICK;
    for (;;) {
        uno_pc64_poll();
        if (pump_input()) { g_dirty = 1; idle = 0; }
        else if (++idle >= 30) {        /* ~0.5 s: blink the caret + tick clock */
            idle = 0; g_dirty = 1;
            if (++halfsecs & 1) {}      /* update the uptime label once a second */
            else { fmt_uptime(halfsecs / 2); }
        }
        feed(&tick);                    /* advance the caret-blink timebase */
        if (UI.full) g_dirty = 1;       /* fullscreen apps redraw every frame */
        if (g_dirty) { unoui_render_ui(&UI); uno_pc64_present(); g_dirty = 0; }
        else uno_pc64_delay_ms(16);
    }
    return 0;
}

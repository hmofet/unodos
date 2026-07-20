/* ===========================================================================
 * UnoDOS/Dreamcast - the unoui SHELL: a real desktop (Aurora default) hosting
 * the full 11-app roster, the console analogue of pc64/pc64_uui.c. Built by
 * `./build.sh dc uui` in place of the legacy Mac-style core (unodos.c).
 *
 *   - unoui owns the desktop / windows / widgets / rendering into fb[] (fb.c,
 *     ARGB8888); this file presents fb[] to the 640x480 RGB565 Dreamcast
 *     framebuffer (vram_s) each vblank and maps the maple controller (+ a DC
 *     keyboard, if plugged) to unoui events.
 *   - a taskbar (Start button + one chip per open window + RTC clock) and a
 *     desktop icon grid launch apps; the Start menu lists the roster + Display
 *     (theme picker) + Exit.
 *   - each roster app is hosted UNCHANGED in a unoui window's UI_CANVAS via
 *     the bridge in dc_uui_apps.c (paint/input mapped through the KernelApi /
 *     mac_compat layer). Sound reaches the AICA through the Sound Manager shim
 *     (mac_io.c -> uno_dc_snd_* below); files persist on the VMU (/vmu/a1).
 *
 * Controller map: d-pad = focus/arrows - A = Enter - B = close window / back -
 * X = Tab (next widget) - Y = cycle windows - Start = Start menu.
 *
 * -DUNO_UUI_AUTOTEST bakes in a self-driving screenshot script (desktop ->
 * Start menu -> keyboard-launch Dostris -> open Files/Paint/Tracker), driven
 * through the same feed_key path the controller uses.
 * ======================================================================== */
#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/video.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <arch/arch.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "uui_apps.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

/* ---- AICA square-wave synth (same voice model as dc_main.c) ---------------
 * mac_io.c's Sound Manager shim calls uno_dc_snd_note/quiet (UNO_DC build);
 * the legacy platform file dc_main.c is not linked here, so provide them. */
#define SQ_PERIOD 32
static sfxhnd_t g_sq = 0;
static int      g_snd_ready = 0;

static void snd_bringup(void)
{
    static char wave[SQ_PERIOD];
    int i;
    if (g_snd_ready) return;
    snd_init();
    for (i = 0; i < SQ_PERIOD; i++) wave[i] = (i < SQ_PERIOD / 2) ? 100 : -100;
    g_sq = snd_sfx_load_raw_buf(wave, SQ_PERIOD, 44100, 8, 1);
    g_snd_ready = 1;
}
static int midi_hz(int midi)
{ return (int)(440.0 * pow(2.0, (midi - 69) / 12.0) + 0.5); }

void uno_dc_snd_note(int chan, int midi, int amp)
{
    sfx_play_data_t d;
    if (!g_snd_ready || !g_sq || midi <= 0) return;
    memset(&d, 0, sizeof d);
    d.chn = chan; d.idx = g_sq;
    d.vol = (amp > 255) ? 255 : (amp < 0 ? 0 : amp);
    d.pan = 128; d.loop = 1;
    d.freq = midi_hz(midi) * SQ_PERIOD;
    snd_sfx_play_ex(&d);
}
void uno_dc_snd_quiet(int chan)
{ if (g_snd_ready) snd_sfx_stop(chan); }

/* ---- themes (Display dropdown + live re-skin) --------------------------- */
static const struct { const char *name; const struct unoui_theme *theme; } kThemes[] = {
    { "Aurora Light", &theme_aurora_light }, { "Aurora Dark", &theme_aurora_dark },
    { "UnoDOS",    &theme_unodos  }, { "Mac OS 7",    &theme_macos7 },
    { "Mac Plus",  &theme_macplus }, { "Windows 3.1", &theme_win31  },
    { "Amiga",     &theme_amiga   }, { "C64",         &theme_c64    },
    { "Apple II",  &theme_apple2  }, { "NeXTSTEP",    &theme_next   }
};
#define NTHEMES ((int)(sizeof kThemes / sizeof kThemes[0]))
static const char *kThemeNames[NTHEMES];

/* ---- apps: the 11 bridge-hosted roster apps + the native Display panel --- */
#define APP_DISPLAY UNOAPP_COUNT
#define NAPPS       (UNOAPP_COUNT + 1)
#define APP_TBAR 18                       /* legacy apps' own title-bar height */

static const char *app_name(int a)
{ return a == APP_DISPLAY ? "Display" : unoapp_name(a); }

/* taskbar height (26 px under the 8px bitmap font) */
static int tb_h(void) { int h = fb_text_h() + 12; return h < 26 ? 26 : h; }
#define TASKH (tb_h())

static unoui_ui     UI;
static unoui_window g_launch;             /* Start menu */
static unoui_window g_desk;               /* bare/bottom: desktop icon layer */
static unoui_window g_task;               /* bare/top: the taskbar */
static unoui_window g_win[NAPPS];
static int          g_built[NAPPS], g_open[NAPPS], g_nopen;
static int          g_dirty = 1;

/* one canvas per bridge app; ctx carries its app index */
static unoui_canvas g_lcanvas[UNOAPP_COUNT];
static int          g_lidx[UNOAPP_COUNT];

/* widget ids */
enum { ID_THEME = 1, ID_ALITE,
       ID_LAUNCH0 = 100 };                /* desktop icons: +app */

static char g_clock[16] = "";

/* ---- window management --------------------------------------------------- */
static int g_launch_open;
static int g_menu_hot;

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

static void clamp_to_workarea(unoui_window *w)
{
    if (w->r.x + w->r.w > FB_W)         w->r.x = FB_W - w->r.w;
    if (w->r.y + w->r.h > FB_H - TASKH) w->r.y = FB_H - TASKH - w->r.h;
    if (w->r.x < 0) w->r.x = 0;
    if (w->r.y < 0) w->r.y = 0;
}

/* window height for `content_h` px of content under the current theme */
static int win_h_for(int content_h)
{
    const unoui_metrics *m = &UI.theme->m;
    return content_h + m->title_h + 2 * m->pad + m->frame_w;
}

/* ---- taskbar: one canvas, drawn + hit-tested by hand --------------------- */
static void toggle_launcher(void);
static void open_app(int a);

#define TB_START_X 4
static int tb_start_w(void)  { return 20 + fb_text_w("Start"); }
static int tb_chip_w(void)   { int w = 24 + fb_text_w("Notepad"); return w < 88 ? 88 : w; }
static int tb_chip_gap(void) { return tb_chip_w() + 4; }
static int tb_chip_x(void)   { return TB_START_X + tb_start_w() + 8; }

/* app index of the focused window, or -1 (highlights its taskbar chip) */
static int focused_app(void)
{
    int i;
    if (UI.focus_win < 0 || UI.focus_win >= UI.nwin) return -1;
    for (i = 0; i < NAPPS; i++) if (&g_win[i] == UI.win[UI.focus_win]) return i;
    return -1;
}

static void tb_panel(int x, int y, int w, int h, fb_px face, int pressed)
{
    const unoui_theme *t = UI.theme;
    fb_fill_rect(x, y, w, h, face);
    fb_frame_rect(x, y, w, h, t->pal.dark);
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
    int cr = bh/2 > 8 ? 8 : bh/2;
    (void)w; (void)ctx;
    if (modern) {                                 /* Aurora: frosted bar + hairline */
        fb_blend_rect(r.x, r.y, r.w, r.h, t->pal.win_bg, 236);
        fb_blend_rect(r.x, r.y, r.w, 1, t->pal.text_dim, 45);
    } else {
        fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.face);
        fb_hline(r.x, r.y, r.w, t->pal.light);
    }
    /* Start button */
    { int bx = r.x + TB_START_X, bw = tb_start_w(), fh = fb_text_h();
      if (modern) fb_round_rect(bx, by, bw, bh, cr, t->pal.accent);
      else        tb_panel(bx, by, bw, bh, t->pal.accent, 0);
      fb_text(bx + (bw - fb_text_w("Start")) / 2, by + (bh - fh) / 2, "Start",
              t->pal.accent_text, -1); }
    /* one chip per open window, highlighted if focused */
    x = r.x + tb_chip_x();
    { int cw = tb_chip_w(), fh = fb_text_h();
      int tray_x = r.x + r.w - (fb_text_w(g_clock) + 16) - 6;
    for (i = 0; i < NAPPS; i++) {
        int d = (i == act) ? 1 : 0;
        if (!g_open[i]) continue;
        if (x + cw > tray_x - 4) break;      /* no room left before the tray */
        if (modern) {
            if (d) { fb_round_rect_a(x, by, cw, bh, cr, t->pal.accent, 48, FB_CORNER_ALL);
                     fb_fill_rect(x + 8, by + bh - 2, cw - 16, 2, t->pal.accent); }
            else     fb_round_rect_a(x, by, cw, bh, cr, t->pal.text, 18, FB_CORNER_ALL);
        } else tb_panel(x, by, cw, bh, t->pal.face, d);
        { int dd = modern ? 0 : d;
          fb_set_clip(x + 4, by, cw - 8, bh);
          fb_text(x + 8 + dd, by + (bh - fh) / 2 + dd, app_name(i), t->pal.text, -1);
          fb_set_clip(r.x, r.y, r.w, r.h); }
        x += tb_chip_gap();
    } }
    /* system tray: the RTC clock chip, right-aligned */
    if (g_clock[0]) {
        int fh = fb_text_h();
        int cw = fb_text_w(g_clock) + 16, cxx = r.x + r.w - cw - 6;
        if (modern) fb_round_rect_a(cxx, by, cw, bh, cr, t->pal.text, 16, FB_CORNER_ALL);
        else        tb_panel(cxx, by, cw, bh, t->pal.field_bg, 1);
        fb_text(cxx + 8, by + (bh - fh) / 2, g_clock,
                modern ? t->pal.text : t->pal.field_text, -1);
    }
}

static int taskbar_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    int px, i, x;
    (void)w; (void)ctx;
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    px = e->x - g_task.r.x;
    if (px >= TB_START_X && px < TB_START_X + tb_start_w()) { toggle_launcher(); return 1; }
    x = tb_chip_x();
    for (i = 0; i < NAPPS; i++) {
        if (!g_open[i]) continue;
        if (px >= x && px < x + tb_chip_w()) { open_app(i); return 1; }
        x += tb_chip_gap();
    }
    return 0;
}
static unoui_canvas g_taskcv = { taskbar_draw, taskbar_event, 0 };

static void build_taskbar(void)
{
    unoui_window_init(&g_task, "", 0, FB_H - TASKH, FB_W, TASKH);
    g_task.flags = UI_WIN_BARE | UI_WIN_TOP;
    unoui_add_canvas(&g_task, 0, 0, FB_W, TASKH, &g_taskcv);
}

static void build_desktop(void)
{
    int i, percol, fh = fb_text_h();
    int ich = 34 + fh, pitch = ich + 8, colw = 20 + fb_text_w("MMMMMMMM");
    unoui_window_init(&g_desk, "", 0, 0, FB_W, FB_H - TASKH);
    g_desk.flags = UI_WIN_BARE | UI_WIN_BOTTOM;
    percol = (FB_H - TASKH - 20) / pitch; if (percol < 1) percol = 1;
    for (i = 0; i < NAPPS; i++) {
        int col = i / percol, row = i % percol;
        unoui_widget *ic = unoui_add_icon(&g_desk, 16 + col * colw, 14 + row * pitch,
                                          app_name(i));
        ic->r.w = colw - 20; ic->r.h = ich;
        ic->icon = i;
        ic->id  = ID_LAUNCH0 + i;
    }
}

/* ---- bridge apps hosted in a canvas -------------------------------------- */
static void lcanvas_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{ (void)w; unoapp_paint(*(int *)ctx, r); }

static int lcanvas_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    (void)w;
    if (unoapp_input(*(int *)ctx, (const unoui_event *)ev)) { g_dirty = 1; return 1; }
    return 0;
}

/* ---- the native Display panel (theme picker) ----------------------------- */
static void build_display(unoui_window *w)
{
    unoui_widget *x; int i;
    int fh = fb_text_h(), ch = ui_field_h();
    int row = ch + 8, y = 4, lw = fb_text_w("Theme:") + 12, cw = 280;
    int lofs = (ch - fh) / 2;
    for (i = 0; i < NTHEMES; i++) kThemeNames[i] = kThemes[i].name;
    unoui_window_init(w, "Display", 300, 120, 1, 1);
    unoui_add_label(w, 8, y + lofs, "Theme:");
    { int cur = 0;
      for (i = 0; i < NTHEMES; i++) if (kThemes[i].theme == UI.theme) cur = i;
      x = unoui_add_dropdown(w, lw, y, cw - lw - 8, kThemeNames, NTHEMES, cur);
      x->id = ID_THEME; }
    y += row;
    x = unoui_add_check(w, 8, y, "Aurora lite (fast compositing)", unoui_aurora_lite);
    x->id = ID_ALITE;
    y += fh + 12;
    unoui_add_sep(w, 8, y, cw - 16); y += 8;
    unoui_add_label(w, 8, y, "X selects the theme box, then");   y += fh + 4;
    unoui_add_label(w, 8, y, "Up/Down changes the theme.");      y += fh + 8;
    w->r.w = cw + 2 * UI.theme->m.frame_w + 2 * UI.theme->m.pad;
    w->r.h = win_h_for(y);
}

static void build_legacy(int a)
{
    int aw, ah;
    const unoui_metrics *m = &UI.theme->m;
    unoui_canvas *cv = &g_lcanvas[a];
    unoapp_size(a, &aw, &ah);
    if (aw < 140) aw = 140;
    if (ah < 100) ah = 100;
    g_lidx[a] = a;
    cv->draw = lcanvas_draw; cv->event = lcanvas_event; cv->ctx = &g_lidx[a];
    /* cascade new windows; content area == the app's drawable area (the app
     * minus its own title bar, which unoui draws instead). Fixed pixel layouts
     * -> non-resizable. */
    unoui_window_init(&g_win[a], app_name(a),
                      16 + (g_nopen % 5) * 26, 10 + (g_nopen % 4) * 22,
                      aw + 2 * m->frame_w + 2 * m->pad,
                      (ah - APP_TBAR) + m->title_h + 2 * m->pad + m->frame_w);
    unoui_add_canvas(&g_win[a], 0, 0, aw, ah - APP_TBAR, cv);
}

static void open_app(int a)
{
    if (a < 0 || a >= NAPPS) return;
    if (!g_built[a]) {
        if (a == APP_DISPLAY) build_display(&g_win[a]);
        else                  build_legacy(a);
        g_built[a] = 1;
    }
    if (!g_open[a]) {
        clamp_to_workarea(&g_win[a]);
        if (!unoui_ui_add(&UI, &g_win[a])) return;    /* window table full */
        g_open[a] = 1; g_nopen++;
        if (a < UNOAPP_COUNT) unoapp_open(a);
    } else raise_win(&g_win[a]);
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }
    { int fi; for (fi = 0; fi < UI.nwin; fi++)
        if (UI.win[fi] == &g_win[a]) { UI.focus_win = fi; break; } }
    if (a < UNOAPP_COUNT) UI.focus_wi = 0;  /* focus the canvas: d-pad -> app */
    g_dirty = 1;
}

/* the KernelApi launch_app hook (an app launching another app) */
void uui_shell_launch(short proc) { open_app(proc); }

static void close_focused(void)
{
    int f = UI.focus_win, i;
    unoui_window *win;
    if (f < 0 || f >= UI.nwin) return;
    win = UI.win[f];
    if (win->flags & UI_WIN_BARE) return;         /* never close desktop/taskbar */
    if (win == &g_launch) { remove_win(&g_launch); g_launch_open = 0; g_dirty = 1; return; }
    for (i = 0; i < NAPPS; i++) if (&g_win[i] == win) {
        g_open[i] = 0;
        if (g_nopen > 0) g_nopen--;
        if (i < UNOAPP_COUNT) unoapp_close(i);
        break;
    }
    remove_win(win);
    g_dirty = 1;
}

/* Y: raise the next OPEN app window (skips desktop + taskbar) */
static void cycle_window(void)
{
    int i, cur = -1;
    if (UI.focus_win >= 0 && UI.focus_win < UI.nwin)
        for (i = 0; i < NAPPS; i++) if (&g_win[i] == UI.win[UI.focus_win]) { cur = i; break; }
    for (i = 1; i <= NAPPS; i++) {
        int a = (cur + i) % NAPPS;
        if (g_open[a]) {
            raise_win(&g_win[a]);
            if (a < UNOAPP_COUNT) UI.focus_wi = 0;
            g_dirty = 1; return;
        }
    }
}

/* ---- Start menu: a keyboard-first canvas (apps + Display + Exit) --------- */
static int mrow_h(void) { int h = fb_text_h() + 8; return h < 22 ? 22 : h; }
#define MROW (mrow_h())

static int  menu_count(void)         { return NAPPS + 1; }   /* + Exit */
static const char *menu_label(int i)
{ return i < NAPPS ? app_name(i) : "Exit to BIOS"; }

static void menu_activate(int i)
{
    if (i < 0 || i >= menu_count()) return;
    if (i < NAPPS) open_app(i);              /* also closes the launcher */
    else           arch_menu();              /* back to the DC boot menu */
}

static void launcher_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = UI.theme; int total = menu_count(), i;
    (void)w; (void)ctx;
    for (i = 0; i < total; i++) {
        int ry = r.y + i * MROW, hot = (i == g_menu_hot);
        if (hot) fb_fill_rect(r.x, ry, r.w, MROW, t->pal.accent);
        if (i == NAPPS) fb_hline(r.x, ry, r.w, t->pal.shadow);   /* power divider */
        fb_text(r.x + 10, ry + (MROW - fb_text_h()) / 2, menu_label(i),
                hot ? t->pal.accent_text : t->pal.text, -1);
    }
}

static int launcher_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; int ox, oy, row;
    (void)w; (void)ctx;
    if (e->kind == UI_EV_KEY) {                       /* d-pad navigation */
        int total = menu_count();
        if (e->key == UI_KEY_DOWN) { if (g_menu_hot < total - 1) g_menu_hot++; g_dirty = 1; return 1; }
        if (e->key == UI_KEY_UP)   { if (g_menu_hot > 0) g_menu_hot--;         g_dirty = 1; return 1; }
        if (e->key == UI_KEY_ENTER){ menu_activate(g_menu_hot); return 1; }
        return 0;
    }
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    unoui_content_origin(UI.theme, &g_launch, &ox, &oy);
    row = (e->y - oy) / MROW;
    if (row >= 0 && row < menu_count()) { menu_activate(row); return 1; }
    return 0;
}
static unoui_canvas g_menu_cv = { launcher_draw, launcher_event, 0 };

static void build_launcher(void)
{
    const unoui_metrics *m = &UI.theme->m;
    int i, tw = 0, winw, total = menu_count();
    for (i = 0; i < total; i++) { int t = fb_text_w(menu_label(i)); if (t > tw) tw = t; }
    winw = tw + 20 + 14 + 2 * m->frame_w + 2 * m->pad;
    g_menu_hot = 0;
    unoui_window_init(&g_launch, "Programs", 8, 20,
                      winw, m->title_h + m->pad + total * MROW + m->pad + m->frame_w);
    unoui_add_canvas(&g_launch, 0, 0, winw - 2 * m->frame_w - 2 * m->pad,
                     total * MROW, &g_menu_cv);
}

static void toggle_launcher(void)
{
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }
    else { g_menu_hot = 0;
           g_launch.r.x = 4; g_launch.r.y = FB_H - TASKH - g_launch.r.h - 4;
           clamp_to_workarea(&g_launch);
           unoui_ui_add(&UI, &g_launch);
           UI.focus_wi = 0;                    /* focus the menu canvas for keys */
           g_launch_open = 1; }
    g_dirty = 1;
}

/* ---- actions ------------------------------------------------------------- */
static void on_action(const unoui_action *a)
{
    if (!a->changed) return;
    g_dirty = 1;
    if (a->kind == UI_ACT_CLOSE) { close_focused(); return; }
    if (a->id >= ID_LAUNCH0 && a->id < ID_LAUNCH0 + NAPPS) { open_app(a->id - ID_LAUNCH0); return; }
    switch (a->id) {
    case ID_THEME: if (a->value >= 0 && a->value < NTHEMES)
                       unoui_ui_theme(&UI, kThemes[a->value].theme);
                   break;
    case ID_ALITE: unoui_aurora_lite = a->value ? 1 : 0; break;
    default: break;
    }
}

static void feed(const unoui_event *ev)
{ unoui_action a = unoui_handle(&UI, ev); on_action(&a); }

static void feed_key(int k)
{ unoui_event e; memset(&e, 0, sizeof e); e.kind = UI_EV_KEY; e.key = k; feed(&e); g_dirty = 1; }
static void feed_ch(int c)
{ unoui_event e; memset(&e, 0, sizeof e); e.kind = UI_EV_CHAR; e.ch = c; feed(&e); g_dirty = 1; }

/* ---- maple input: controller (primary) + keyboard (typed text) -----------
 * d-pad -> arrows, A -> Enter, B -> close/back, X -> Tab, Y -> cycle windows,
 * Start -> Start menu. Edge-detected. */
static void poll_controller(uint32 *prev)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st;
    uint32 now, edge;
    if (!dev) return;
    st = (cont_state_t *)maple_dev_status(dev);
    if (!st) return;
    now  = st->buttons;
    edge = now & ~(*prev);              /* newly pressed this frame */
    *prev = now;
    if (edge & CONT_DPAD_LEFT)  feed_key(UI_KEY_LEFT);
    if (edge & CONT_DPAD_RIGHT) feed_key(UI_KEY_RIGHT);
    if (edge & CONT_DPAD_UP)    feed_key(UI_KEY_UP);
    if (edge & CONT_DPAD_DOWN)  feed_key(UI_KEY_DOWN);
    if (edge & CONT_A)          feed_key(UI_KEY_ENTER);
    if (edge & CONT_X)          feed_key(UI_KEY_TAB);
    if (edge & CONT_Y)          cycle_window();
    if (edge & CONT_B) {
        if (g_launch_open) toggle_launcher();
        else               close_focused();
        g_dirty = 1;
    }
    if (edge & CONT_START)      toggle_launcher();
}

static void poll_keyboard(void)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    int k;
    if (!dev) return;
    while ((k = kbd_queue_pop(dev, 1)) > 0) {
        if (k == 27)                 { if (g_launch_open) toggle_launcher();
                                       else close_focused(); g_dirty = 1; }
        else if (k == 13 || k == 10) feed_key(UI_KEY_ENTER);
        else if (k == 8)             feed_key(UI_KEY_BACKSPACE);
        else if (k == 9)             feed_key(UI_KEY_TAB);
        else if (k >= 32 && k < 127) feed_ch(k);
    }
}

/* ---- present: fb[] (ARGB8888) -> RGB565 vram, converted only when drawn --- */
static inline uint16 to565(fb_px p)
{
    unsigned r = (p) & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
    return (uint16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static void present(void)
{
    uint16 *vram = (uint16 *)vram_s;
    int i, n = FB_W * FB_H;
    for (i = 0; i < n; i++) vram[i] = to565(fb[i]);
}

/* ---- taskbar clock: the Dreamcast RTC ------------------------------------ */
static void fmt_clock(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char nc[16];
    if (tm) {
        nc[0]='0'+(tm->tm_hour/10)%10; nc[1]='0'+tm->tm_hour%10; nc[2]=':';
        nc[3]='0'+(tm->tm_min /10)%10; nc[4]='0'+tm->tm_min %10; nc[5]=':';
        nc[6]='0'+(tm->tm_sec /10)%10; nc[7]='0'+tm->tm_sec %10; nc[8]=0;
    } else nc[0] = 0;
    if (strcmp(nc, g_clock)) { strcpy(g_clock, nc); g_dirty = 1; }
}

/* ---- self-driving screenshot script (-DUNO_UUI_AUTOTEST) ------------------
 * Drives the SAME feed_key path the controller uses:
 *   ~4 s  Start menu opens              (shot: launcher over the desktop)
 *   ~15 s 5 x Down + Enter              (keyboard-launches Dostris)
 *   ~16-18 s Files, Paint, Tracker open (shot: the roster desktop)         */
#ifdef UNO_UUI_AUTOTEST
static void cycle_window(void);
static void autotest(unsigned f)
{
    int i;
    switch (f) {
    case 240:  toggle_launcher(); break;                  /* menu shot window */
    case 900:  for (i = 0; i < 2; i++) feed_key(UI_KEY_DOWN);
               feed_key(UI_KEY_ENTER); break;             /* menu-launch Files */
    case 960:  open_app(9);  break;   /* Paint   (uno_app.h proc order) */
    case 1020: open_app(8);  break;   /* Tracker */
    case 1080: open_app(5);  break;   /* Dostris (focused: canvas gets keys) */
    case 1140: feed_ch('n'); break;             /* start the Dostris game */
    default:                          /* then cycle the stack every ~5 s so a
                                         series of captures shows every app */
        if (f >= 1800 && (f % 300) == 0) cycle_window();
        break;
    }
}
#endif

/* the bridge app whose window is focused (fed ticks + game music), or -1 */
static int active_app(void)
{
    int a;
    unoui_window *win = (UI.focus_win >= 0 && UI.focus_win < UI.nwin)
                        ? UI.win[UI.focus_win] : 0;
    if (win) for (a = 0; a < UNOAPP_COUNT; a++)
        if (&g_win[a] == win && g_open[a]) return a;
    return -1;
}

int main(int argc, char **argv)
{
    uint32 prev = 0;
    unsigned frame = 0;
    unoui_event tick;
    (void)argc; (void)argv;

    vid_set_mode(DM_640x480, PM_RGB565); /* KOS auto-selects VGA vs NTSC/PAL */
    vid_empty();
    snd_bringup();                       /* AICA up for Music/Tracker/games  */

    unoui_ui_init(&UI, &theme_aurora_light, FB_W, FB_H);
    unoapp_setup(&g_dirty);              /* wire the roster's KernelApi      */
    build_desktop();  unoui_ui_add(&UI, &g_desk);
    build_taskbar();  unoui_ui_add(&UI, &g_task);
    build_launcher();
    fmt_clock();

    memset(&tick, 0, sizeof tick); tick.kind = UI_EV_TICK;
    for (;;) {
        int la;
        poll_controller(&prev);
        poll_keyboard();
#ifdef UNO_UUI_AUTOTEST
        autotest(frame);
#endif
        feed(&tick);                     /* caret blink timebase */
        la = active_app();               /* drive the focused app's clock */
        unoapp_focus(la);
        if (la >= 0) unoapp_run_tick(la);
        if ((frame & 31) == 0) fmt_clock();
        if (g_dirty) {
            unoui_render_ui(&UI);
#ifdef UNO_UUI_AUTOTEST
            {   /* focus debug overlay: focused window index / widget index /
                   active app - lands in the Flycast screenshot itself */
                char d[48]; int n = 0, la2 = active_app();
                const char *pre = "fw="; while (*pre) d[n++] = *pre++;
                d[n++] = (char)('0' + ((UI.focus_win + 10) / 10) % 10);
                d[n++] = (char)('0' + (UI.focus_win + 10) % 10);
                pre = " wi="; while (*pre) d[n++] = *pre++;
                d[n++] = (char)('0' + ((UI.focus_wi + 10) / 10) % 10);
                d[n++] = (char)('0' + (UI.focus_wi + 10) % 10);
                pre = " la="; while (*pre) d[n++] = *pre++;
                d[n++] = (char)('0' + ((la2 + 10) / 10) % 10);
                d[n++] = (char)('0' + (la2 + 10) % 10);
                d[n] = 0;                       /* values are +10 biased */
                fb_text(FB_W - 150, 2, d, FB_RGB(255, 0, 0), FB_RGB(0, 0, 0));
            }
#endif
            present();
            g_dirty = 0;
        }
        vid_waitvbl();
        frame++;
    }
    return 0;
}

/* ===========================================================================
 * uc_main.c - the module entry point, the workbench layout and input routing.
 *
 * UnoCode is a unoui-CLASS module (APPS\UNOCODE.UNO): the shell owns the
 * window, we own its content.  Everything below the window - the toolkit, the
 * framebuffer, the TrueType text path, the filesystem, malloc, unojs - arrives
 * through the named kernel exports in pc64_modload.c, so this module carries no
 * kernel code and a distro drops UnoCode by not shipping the file.
 *
 * INPUT ARRIVES BY TWO ROADS and this file is where they meet.
 *
 *   1. The CANVAS event stream (unoui_event) carries the mouse, the wheel, the
 *      navigation keys and printable characters, WITH modifiers.
 *   2. The module `key(uni, scan, ctrl)` hook carries Ctrl chords and the
 *      function keys, which never become unoui events at all.
 *
 * The second road has no Shift flag - but the transports report the SHIFTED
 * CHARACTER, so Ctrl+Shift+P arrives as uni='P' where Ctrl+P arrives as 'p',
 * and that is how Shift is recovered.  One honest limit follows and is written
 * down rather than discovered: Alt chords cannot be seen on that road, so the
 * Alt bindings (Alt+Up/Down to move a line) work through the canvas keys,
 * where the modifier mask is real.
 * ======================================================================== */
#include "unocode.h"
#include "uno_uuiapp.h"
#include "uno_appdesc.h"

#ifdef UNO_APP_SYM
#  define uno_app_main UNO_APP_SYM
#endif

UcWorkbench UC;

static unoui_window *g_win;
static char g_status[120];
static unsigned long g_status_until;
static int  g_inited;

/* EFI SimpleTextIn scan codes, which is the space the shell's key hook speaks */
#define K_F1  0x0B
#define K_F12 0x16
#define UC_KEY_F1 0x200

/* ---- small services the whole module uses --------------------------------- */
void uc_repaint(void) { pc64_shell_dirty(); }

void uc_status_msg(const char *s)
{
    uc_scpy(g_status, s ? s : "", sizeof g_status);
    g_status_until = uno_dbg_uptime_ms() + 6000;
    uc_repaint();
}

const char *uc_status_msg_get(void)
{
    if (uno_dbg_uptime_ms() > g_status_until) return "";
    return g_status;
}

int uc_path_join(char *out, int cap, const char *dir, const char *name)
{
    out[0] = 0;
    if (dir && dir[0]) {
        uc_scpy(out, dir, cap);
        uc_scat(out, "\\", cap);
    }
    uc_scat(out, name ? name : "", cap);
    return (int)strlen(out);
}

/* Read a whole file into a NUL-terminated malloc'd buffer.  Every reader in
 * UnoCode goes through this one: the terminating NUL matters (uc_json_parse
 * and the document loader both rely on it) and a 0-byte file must come back as
 * a valid empty buffer rather than as a failure, or an empty settings.json
 * reads as "no settings file" and the defaults silently come back. */
int uc_read_file(int vol, const char *path, char **out, long *len)
{
    long n;
    char *buf;
    *out = 0;
    if (len) *len = 0;
    if (vol < 0 || !path || !path[0]) return 0;
    n = uno_fs_size(vol, path);
    if (n < 0) return 0;
    if (n > 4L * 1024 * 1024) return 0;
    buf = (char *)malloc((unsigned long)n + 2);
    if (!buf) return 0;
    if (n > 0 && uno_fs_read(vol, path, (unsigned char *)buf, n) < 0) { free(buf); return 0; }
    buf[n] = 0;
    buf[n + 1] = 0;
    *out = buf;
    if (len) *len = n;
    return 1;
}

/* ---- listing a directory, INCLUDING its subdirectories ---------------------
 * uno_fs_list_dir() and uno_fs_list_begin() both report FILES ONLY.  That is
 * unofs's documented behaviour and it is right for their callers, but a file
 * TREE that cannot see folders is not a file tree - and a folder containing
 * only folders comes back EMPTY, which is exactly how EXT\ (three extension
 * directories and not one file) first reported "no extensions installed".
 *
 * On a native FAT volume the richer uno_fat_list_ex() answers with the dir
 * flag and the size.  On the firmware SFS volume there is no equivalent, so
 * entries fall back to uno_fs_list_dir() and are probed with uno_fs_isdir(),
 * which that backing cannot answer either - there, a directory reads as a file
 * and opening it fails cleanly rather than silently doing nothing. */
int uc_list_dir(int vol, const char *dir, char (*names)[16],
                unsigned char *isdir, int maxn)
{
    int fi, n, i;
    if (vol < 0 || maxn <= 0) return 0;
    fi = uno_fs_fat_index(vol);
    if (fi >= 0) {
        /* static, and safe under the Explorer's recursion: every entry is
         * copied into the caller's own array before this returns, so a nested
         * call reusing the buffer cannot reach data anybody still holds. */
        static uno_fat_entry ents[128];
        int cap = maxn > 128 ? 128 : maxn;
        n = uno_fat_list_ex(fi, dir ? dir : "", ents, cap);
        if (n > cap) n = cap;
        for (i = 0; i < n; i++) {
            uc_scpy(names[i], ents[i].name, 16);
            if (isdir) isdir[i] = (unsigned char)(ents[i].is_dir != 0);
        }
        return n;
    }
    if (dir && dir[0]) n = uno_fs_list_dir(vol, dir, names, maxn);
    else {
        n = uno_fs_list_begin(vol);
        if (n > maxn) n = maxn;
        for (i = 0; i < n; i++) uno_fs_list_get(vol, i, names[i], 16);
    }
    if (n > maxn) n = maxn;
    for (i = 0; i < n; i++) {
        char full[UC_PATH_MAX];
        uc_path_join(full, sizeof full, dir, names[i]);
        if (isdir) isdir[i] = (unsigned char)uno_fs_isdir(vol, full);
    }
    return n;
}

/* ---- layout ---------------------------------------------------------------- */
void uc_layout(void)
{
    UcRect c = UC.canvas;
    int act = uc_cfg_bool("workbench.activityBar.visible") ? 48 : 0;
    int side = UC.sidebar_visible ? UC.sidebar_w : 0;
    int status = uc_cfg_bool("workbench.statusBar.visible") ? uc_ui_h() + 8 : 0;
    int tabs = uc_cfg_bool("workbench.editor.showTabs") ? uc_ui_h() + 14 : 0;
    int crumbs = uc_cfg_bool("breadcrumbs.enabled") ? uc_ui_h() + 6 : 0;
    int panel = UC.panel_visible ? UC.panel_h : 0;
    int right = !strcmp(uc_cfg_str("workbench.sideBar.location"), "right");
    int body_h = c.h - status;
    int x = c.x;

    /* The side bar is a FRACTION of the window until somebody drags it.  A
     * fixed 210 px is right on the 640x400 default desktop and mean on a
     * 1280x800 one, where it clips every extension's description while a third
     * of the window sits empty. */
    if (!UC.sidebar_user && c.w > 0) {
        int want = c.w * 22 / 100;
        if (want < 180) want = 180;
        if (want > 320) want = 320;
        UC.sidebar_w = want;
        side = UC.sidebar_visible ? want : 0;
    }
    if (UC.zen) { act = 0; side = 0; tabs = 0; crumbs = 0; panel = 0; }
    if (side > c.w - act - 200) side = c.w - act - 200;
    if (side < 0) side = 0;
    if (panel > body_h - 120) panel = body_h - 120;
    if (panel < 0) panel = 0;

    if (right) {
        UC.activity = (UcRect){ c.x + c.w - act, c.y, act, body_h };
        UC.sidebar  = (UcRect){ c.x + c.w - act - side, c.y, side, body_h };
        x = c.x;
    } else {
        UC.activity = (UcRect){ c.x, c.y, act, body_h };
        UC.sidebar  = (UcRect){ c.x + act, c.y, side, body_h };
        x = c.x + act + side;
    }
    {
        int w = c.w - act - side;
        UC.tabs   = (UcRect){ x, c.y, w, tabs };
        UC.crumbs = (UcRect){ x, c.y + tabs, w, crumbs };
        UC.editor = (UcRect){ x, c.y + tabs + crumbs, w,
                              body_h - tabs - crumbs - panel };
        UC.panel  = (UcRect){ x, c.y + body_h - panel, w, panel };
    }
    UC.status = (UcRect){ c.x, c.y + body_h, c.w, status };
    if (UC.editor.h < 40) UC.editor.h = 40;
}

void uc_focus(int what)
{
    UC.focus = what;
    uc_ctx_refresh();
}

/* Ctrl+Shift+E and Ctrl+` are three-state, as they are in VS Code: show it,
 * then FOCUS it, then hide it.  A plain two-state toggle means the second
 * press of "show the terminal" hides the terminal you were about to type in -
 * which is exactly what the gate script did to itself. */
void uc_toggle_sidebar(int view)
{
    if (view < 0) UC.sidebar_visible = !UC.sidebar_visible;
    else if (UC.sidebar_visible && UC.view == view && UC.focus == UC_F_SIDEBAR)
        UC.sidebar_visible = 0;
    else { UC.sidebar_visible = 1; UC.view = view; uc_focus(UC_F_SIDEBAR); }
    if (UC.sidebar_visible && UC.view == UC_VIEW_EXPLORER) uc_explorer_refresh();
    uc_layout();
    uc_repaint();
}

void uc_toggle_panel(int tab)
{
    if (tab < 0) UC.panel_visible = !UC.panel_visible;
    else if (UC.panel_visible && UC.panel_tab == tab && UC.focus == UC_F_PANEL)
        UC.panel_visible = 0;
    else { UC.panel_visible = 1; UC.panel_tab = tab; uc_focus(UC_F_PANEL); }
    if (UC.panel_visible && UC.panel_tab == UC_PANEL_TERMINAL) uc_term_init();
    uc_layout();
    uc_repaint();
}

void uc_open_folder(int vol, const char *dir)
{
    UC.ws_vol = vol;
    uc_scpy(UC.ws_dir, dir ? dir : "", sizeof UC.ws_dir);
    uc_explorer_refresh();
    uc_tasks_reload();
    uc_repaint();
}

/* ---- painting --------------------------------------------------------------- */
static void uc_draw(unoui_widget *w, unoui_rect r, void *ctx)
{
    (void)w; (void)ctx;
    UC.canvas = (UcRect){ r.x, r.y, r.w, r.h };
    uc_layout();

    if (UC.activity.w) uc_activity_draw(UC.activity);
    if (UC.sidebar.w)  uc_sidebar_draw(UC.sidebar);
    if (UC.tabs.h)     uc_tabs_draw(UC.tabs);
    if (UC.crumbs.h)   uc_breadcrumb_draw(UC.crumbs);
    uc_edit_draw(UC.editor, uc_doc_active(), UC.focus == UC_F_EDITOR);
    if (UC.panel.h)    uc_panel_draw(UC.panel);
    if (UC.status.h)   uc_status_draw(UC.status);
    uc_notif_draw(UC.canvas);
    uc_quick_draw(UC.canvas);
}

/* ---- pointer routing ---------------------------------------------------------- */
static int hit(UcRect r, int x, int y)
{ return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static int handle_mouse(const unoui_event *e)
{
    UC.mouse_x = e->x;
    UC.mouse_y = e->y;

    if (e->kind == UI_EV_MOUSE_UP) {
        UC.mouse_down = 0;
        if (UC.drag) { UC.drag = UC_DRAG_NONE; return 1; }
    }
    /* the two splitters, tested before anything they sit between */
    if (e->kind == UI_EV_MOUSE_DOWN) {
        UC.mouse_down = 1;
        if (UC.sidebar.w &&
            e->x >= UC.sidebar.x + UC.sidebar.w - 3 && e->x <= UC.sidebar.x + UC.sidebar.w + 2 &&
            hit((UcRect){UC.sidebar.x, UC.sidebar.y, UC.sidebar.w + 4, UC.sidebar.h}, e->x, e->y)) {
            UC.drag = UC_DRAG_SIDEBAR;
            return 1;
        }
        if (UC.panel.h && e->y >= UC.panel.y - 3 && e->y <= UC.panel.y + 2) {
            UC.drag = UC_DRAG_PANEL;
            return 1;
        }
    }
    if (e->kind == UI_EV_MOUSE_MOVE) {
        if (UC.drag == UC_DRAG_SIDEBAR) {
            UC.sidebar_user = 1;               /* stop auto-sizing it */
            UC.sidebar_w = e->x - UC.sidebar.x;
            if (UC.sidebar_w < 120) UC.sidebar_w = 120;
            if (UC.sidebar_w > UC.canvas.w - 260) UC.sidebar_w = UC.canvas.w - 260;
            uc_layout();
            return 1;
        }
        if (UC.drag == UC_DRAG_PANEL) {
            UC.panel_h = UC.status.y - e->y;
            if (UC.panel_h < 80) UC.panel_h = 80;
            if (UC.panel_h > UC.canvas.h - 160) UC.panel_h = UC.canvas.h - 160;
            uc_layout();
            return 1;
        }
    }

    if (uc_quick_active() && uc_quick_event(UC.canvas, e)) return 1;
    if (uc_find_active() && uc_find_event(UC.editor, e)) return 1;

    if (hit(UC.activity, e->x, e->y) && e->kind == UI_EV_MOUSE_DOWN) {
        int v = uc_activity_hit(UC.activity, e->x, e->y);
        if (v == -2) uc_cmd_run("workbench.action.openSettingsJson");
        else if (v >= 0) uc_toggle_sidebar(v);
        return 1;
    }
    if (hit(UC.sidebar, e->x, e->y)) return uc_sidebar_event(UC.sidebar, e);
    if (hit(UC.tabs, e->x, e->y))    return uc_tabs_event(UC.tabs, e);
    if (hit(UC.panel, e->x, e->y))   return uc_panel_event(UC.panel, e);
    if (hit(UC.status, e->x, e->y))  return uc_status_event(UC.status, e);
    if (hit(UC.editor, e->x, e->y) || UC.drag) {
        if (e->kind == UI_EV_MOUSE_DOWN) uc_focus(UC_F_EDITOR);
        return uc_edit_event(UC.editor, uc_doc_active(), e);
    }
    return 0;
}

/* ---- keyboard routing ----------------------------------------------------------- */
static int route_key(int key, int mods)
{
    UcDoc *d = uc_doc_active();
    if (uc_quick_active()) return uc_quick_key(key, mods, 0);
    if (uc_keys_dispatch(key, mods)) return 1;
    if (uc_find_active() && (UC.focus == UC_F_EDITOR)) {
        /* the find box owns typing while it is open, but not the arrows -
         * scrolling the document with the widget up is the whole point */
        if (key != UI_KEY_UP && key != UI_KEY_DOWN && key != UI_KEY_PGUP &&
            key != UI_KEY_PGDN)
            return uc_find_key(key, mods, 0);
    }
    if (UC.focus == UC_F_SIDEBAR && uc_sidebar_key(key, mods, 0)) return 1;
    if (UC.focus == UC_F_PANEL && uc_panel_key(key, mods, 0)) return 1;
    if (UC.focus == UC_F_EDITOR && d) {
        if (uc_edit_key(d, key, mods)) { uc_edit_reveal(UC.editor, d); return 1; }
    }
    return 0;
}

static int route_char(int ch)
{
    UcDoc *d = uc_doc_active();
    if (uc_quick_active()) return uc_quick_key(0, 0, ch);
    if (uc_find_active() && UC.focus == UC_F_EDITOR) return uc_find_key(0, 0, ch);
    if (UC.focus == UC_F_SIDEBAR) return uc_sidebar_key(0, 0, ch);
    if (UC.focus == UC_F_PANEL) return uc_panel_key(0, 0, ch);
    if (UC.focus == UC_F_EDITOR && d && !d->readonly) {
        if (uc_edit_char(d, ch)) { uc_edit_reveal(UC.editor, d); return 1; }
    }
    return 0;
}

static int uc_event(unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    int took = 0;
    (void)w; (void)ctx;
    switch (e->kind) {
    case UI_EV_MOUSE_DOWN:
    case UI_EV_MOUSE_UP:
    case UI_EV_MOUSE_MOVE:
    case UI_EV_WHEEL:
        took = handle_mouse(e);
        break;
    case UI_EV_KEY:
        took = route_key(e->key, e->mods);
        break;
    case UI_EV_CHAR:
        took = route_char(e->ch);
        break;
    default:
        break;
    }
    if (took) uc_repaint();
    return took;
}

/* The shell's module key hook: Ctrl chords and the function keys, which never
 * become unoui events.  See the file header for how Shift is recovered. */
static int uc_key_hook(int uni, int scan, int ctrl)
{
    int key = 0, mods = 0;
    if (scan >= K_F1 && scan <= K_F12) {
        key = UC_KEY_F1 + (scan - K_F1);
    } else if (uni >= 32 && uni < 127) {
        if (!ctrl) return 0;            /* plain typing goes down the other road */
        key = uni;
        if (key >= 'A' && key <= 'Z') { key += 32; mods |= UI_MOD_SHIFT; }
    } else if (uni > 0 && uni < 32 && ctrl) {
        /* a transport that folds Ctrl+letter to a control code: recover the
         * letter.  Shift is unknowable here, which is why the shifted-letter
         * road above is the one the default keymap is written against. */
        key = uni + 'a' - 1;
    } else return 0;
    if (ctrl) mods |= UI_MOD_CTRL;

    if (uc_quick_active()) { uc_quick_key(key, mods, 0); uc_repaint(); return 1; }
    if (uc_keys_dispatch(key, mods)) { uc_repaint(); return 1; }
    if (uc_find_active() && key == UI_KEY_ESC) { uc_find_close(); uc_repaint(); return 1; }
    return 0;
}

/* ---- lifecycle --------------------------------------------------------------------- */
static unoui_canvas g_canvas = { uc_draw, uc_event, 0 };

static void uc_build(unoui_window *win)
{
    const struct unoui_theme *t = pc64_shell_theme();
    const unoui_metrics *m = &t->m;
    int aw, ah;
    g_win = win;
    uc_metrics_init();
    aw = fb_width() - 60;
    if (aw > 1180) aw = 1180;
    if (aw < 560) aw = 560;
    ah = fb_height() - 70;
    if (ah > 760) ah = 760;
    if (ah < 360) ah = 360;
    unoui_window_init(win, "UnoCode", 18, 14,
                      aw + 2 * m->frame_w + 2 * m->pad,
                      ah + m->title_h + 2 * m->pad + m->frame_w);
    unoui_add_canvas(win, 0, 0, aw, ah, &g_canvas);
    win->flags |= UI_WIN_RESIZE;
    win->min_w = 520;
    win->min_h = 320;
    unoui_widget_fill(&win->w[0]);
}

static int uc_action(const unoui_action *a) { (void)a; return 0; }

static void uc_opened(void)
{
    if (!g_inited) {
        g_inited = 1;
        UC.sidebar_w = 210;
        UC.panel_h = 190;
        UC.sidebar_visible = 1;
        UC.panel_visible = 0;
        UC.view = UC_VIEW_EXPLORER;
        UC.panel_tab = UC_PANEL_TERMINAL;
        UC.focus = UC_F_EDITOR;
        UC.ws_vol = uno_fs_pref_vol();
        UC.ws_dir[0] = 0;

        uc_cfg_init();
        uc_theme_init();
        uc_theme_select(uc_cfg_str("workbench.colorTheme"));
        uc_lang_init();
        uc_metrics_init();
        uc_view_init();
        uc_cmd_init();
        uc_keys_load();
        uc_ext_init();
        /* AGAIN, now that extensions have registered theirs: the saved theme
         * may be one an extension contributes, and at the first attempt above
         * nothing had scanned EXT\ yet.  Selecting a name that does not exist
         * is a no-op, so the first call is not wasted - it is what gets the
         * built-in themes right if the scan finds nothing. */
        uc_theme_select(uc_cfg_str("workbench.colorTheme"));
        uc_explorer_refresh();
        uc_tasks_reload();
        uc_ext_activate_startup();
        if (!strcmp(uc_cfg_str("workbench.startupEditor"), "welcome"))
            uc_cmd_run("unocode.showWelcome");
        uc_status_msg("Ctrl+Shift+P for every command");
    }
    uc_metrics_init();
    uc_ctx_refresh();
}

static void uc_closed(void)
{
    /* the documents stay: closing the window is not closing the workspace, and
     * a reopened UnoCode with the same editors is what the user expects */
    uc_quick_close();
    uc_find_close();
    uc_suggest_close();
}

static int uc_canvas_index(void) { return 0; }

static void uc_frame(void)
{
    static unsigned long last_blink;
    unsigned long phase = uno_dbg_uptime_ms() / 530;
    uc_notif_tick();
    uc_api_pump();
    /* Repaint on the CARET's cadence, not on the frame's: a blinking caret
     * needs two repaints a second, and asking for one every frame would keep
     * the whole desktop compositing for no visible difference. */
    if (phase != last_blink) {
        last_blink = phase;
        uc_repaint();
    }
}

/* what the shell shows for this app (uno_appdesc.h) */
UNO_APP_DESC("id: unocode\n"
             "name: UnoCode\n"
             "short: UnoCode\n"
             "icon: unocode\n"
             "cat: tools\n"
             "rank: 9\n"
             "min: 700x460\n");

static const UnoUuiApp kUnoCode = {
    UNO_UUIAPP_ABI, "UnoCode",
    uc_build, uc_action, uc_key_hook, uc_frame,
    uc_opened, uc_closed, uc_canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved) { (void)reserved; return &kUnoCode; }

/* ---- host queries ----------------------------------------------------------
 * See unocode.h.  These exist for a hosted platform - one with an OS window of
 * its own - and are the alternative to that host duplicating this file's
 * layout arithmetic and this module's document list, both of which would then
 * drift the first time either changed. */

/* The DOCUMENT half of the title only.  The folder half is the host's: the
 * core addresses the workspace as a volume number and has never been told what
 * the OS calls it, so it would have to put "WORK" there - which is a label
 * this module invented, not a folder the user recognises. */
const char *uc_host_title(void)
{
    static char buf[48];
    UcDoc *d = uc_doc_active();
    buf[0] = 0;
    if (!d) return buf;
    uc_scpy(buf, d->name[0] ? d->name : "Untitled", sizeof buf);
    if (d->dirty) uc_scat(buf, " \xe2\x97\x8f", sizeof buf);   /* U+25CF, the dirty dot */
    return buf;
}

int uc_host_dirty_count(void)
{
    int i, n = 0;
    for (i = 0; i < uc_doc_count(); i++) if (uc_doc_at(i)->dirty) n++;
    return n;
}

/* Saves what CAN be saved.  An untitled editor has no path to save to, so it
 * is left alone and counted as unsaved - a host that treated the return as
 * "everything is safe now" would otherwise discard it. */
int uc_host_save_all(void)
{
    int i, left = 0;
    for (i = 0; i < uc_doc_count(); i++) {
        UcDoc *d = uc_doc_at(i);
        if (!d->dirty) continue;
        if (d->name[0]) uc_doc_save(d);
        if (d->dirty) left++;
    }
    return left;
}

int uc_host_cursor_at(int x, int y)
{
    UcDoc *d;
    /* mid-drag the pointer keeps the splitter's shape wherever it has got to */
    if (UC.drag == UC_DRAG_SIDEBAR) return UC_CUR_WE;
    if (UC.drag == UC_DRAG_PANEL)   return UC_CUR_NS;
    /* the same bands uc_canvas_event() grabs on, so what the pointer PROMISES
     * and what a click DOES cannot disagree */
    if (UC.sidebar.w &&
        x >= UC.sidebar.x + UC.sidebar.w - 3 && x <= UC.sidebar.x + UC.sidebar.w + 2 &&
        y >= UC.sidebar.y && y < UC.sidebar.y + UC.sidebar.h) return UC_CUR_WE;
    if (UC.panel.h && y >= UC.panel.y - 3 && y <= UC.panel.y + 2 &&
        x >= UC.panel.x && x < UC.panel.x + UC.panel.w) return UC_CUR_NS;
    if (UC.panel_visible && UC.panel_tab == UC_PANEL_TERMINAL &&
        x >= UC.panel.x && x < UC.panel.x + UC.panel.w &&
        y >= UC.panel.y && y < UC.panel.y + UC.panel.h) return UC_CUR_TEXT;
    d = uc_doc_active();
    if (d && uc_edit_over_text(UC.editor, x, y)) return UC_CUR_TEXT;
    return UC_CUR_ARROW;
}

int uc_host_tab_count(void) { return uc_doc_count(); }

int uc_host_tab_info(int i, int *vol, char *dir, int dcap, char *name, int ncap)
{
    UcDoc *d = uc_doc_at(i);
    if (!d || !d->name[0]) return 0;          /* untitled: nowhere to reopen */
    if (vol) *vol = d->vol;
    if (dir)  uc_scpy(dir, d->dir, dcap);
    if (name) uc_scpy(name, d->name, ncap);
    return 1;
}

/* ===========================================================================
 * uodesk.c - UnoWord / UnoCalc / UnoShow as native Windows, macOS and Linux
 * applications.                                            [EXPERIMENTAL]
 *
 * This is a SHELL, not a port of the apps.  pc64/apps/uo*.c are compiled
 * exactly as build.sh compiles them into APPS\UO*.UNO, and they are hosted
 * through the same UnoUuiApp vtable pc64_uui.c drives.  What this file
 * supplies is what the pc64 shell supplies: a framebuffer, the event pump,
 * and the handful of pc64_shell_* services a module imports by name.
 *
 * Two layers, one rule.  The window's CONTENTS and the place they are drawn
 * are ours: pc64/fb.c's software framebuffer, painted by unoui and the
 * uoffice chrome, identical on every OS.  Everything that belongs to the OS
 * - the top-level window that shows those pixels, keyboard and mouse input,
 * the title, fullscreen, the file picker - comes from that OS's own API
 * through uodesk_plat.h (plat_win32.c / plat_cocoa.m / plat_x11.c).  No
 * third-party toolkit sits between the two.
 *
 * The window IS the frame.  pc64 puts a module in a unoui window with a title
 * bar; on a desktop OS the OS already drew one, so the app's window is held
 * in unoui's FULLSCREEN mode permanently - its canvas owns the whole
 * framebuffer and every event, which is the path UnoShow's slide show and
 * every native game already exercise on pc64.  Resizing the OS window is then
 * just a new screen size.
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uno_uuiapp.h"
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "pc64_font.h"
#include "uofile.h"
#include "uoapp.h"
#include "uodesk.h"
#include "uodesk_plat.h"

const UnoUuiApp *uno_app_main(void *reserved);   /* the app this binary is */

/* ---- the framebuffer size (pc64: uefi_main.c owns these) ---------------- */
int uno_fb_w = 1024, uno_fb_h = 720;

static unoui_ui      UI;
static unoui_window  g_win;
static const UnoUuiApp *g_app;
static int           g_dirty = 1;
static int           g_mx, g_my;         /* the pointer, for the wheel       */
static int           g_kick;             /* a native dialog just closed      */
static char          g_title[256];       /* what the OS title bar last got   */
static int           g_show_full;        /* the slide show has the monitor   */
static int           g_running = 1;

/* ---- the shell services a module imports by name ------------------------ */
void pc64_shell_dirty(void)       { g_dirty = 1; }
int  pc64_shell_workarea_w(void)  { return FB_W; }
int  pc64_shell_workarea_h(void)  { return FB_H; }

/* UnoShow's slide show.  On pc64 this toggles unoui's fullscreen; here unoui
 * is fullscreen ALREADY (see the header), so what the show actually wants -
 * the whole monitor - is the OS window going fullscreen.  Leaving (w == 0)
 * must not drop unoui out of fullscreen: that would reveal a desktop this
 * shell does not have. */
void pc64_shell_fullscreen(unoui_window *w)
{
    g_show_full = w != 0;
    plat_set_fullscreen(g_show_full);
    g_dirty = 1;
}
/* pc64's answer is "a module has put itself fullscreen", i.e. the show -
 * not whether the user pressed F11 */
int pc64_shell_is_fullscreen(void) { return g_show_full; }

/* ---- File > Open / Save As: the OS's own picker ---------------------------
 * Installed into uofile (uof_set_native).  The chosen file's folder becomes
 * a volume, so the app goes on addressing it as (volume, name) exactly as it
 * does on pc64 - and a later plain Save goes back to the same folder. */
static int native_picker(int save, const char *const *types, int ntypes,
                         int *vol, char *name, int cap, int *type)
{
    char path[1024];
    int r = plat_file_dialog(save, types, ntypes, path, (int)sizeof path, type);
    if (r <= 0) { g_kick = r == 0; return r; }
    *vol = uodesk_fs_volume_of(path, name, cap);
    g_kick = 1;
    return *vol >= 0 ? 1 : 0;
}

/* ---- what the app asks of the host (uoapp.h) ---------------------------------
 * Quit is the guard's to call, once the document is saved or the user said
 * not to; the close box itself only ASKS (dispatch, PE_QUIT). */
static void host_quit(void) { g_running = 0; }
static void host_modified(int on) { plat_set_modified(on); }
static int  g_key_mods;           /* the modifiers of the key being delivered */
static int  host_mods(void) { return g_key_mods; }
static const uoa_host kHost = { host_quit, plat_clip_set, plat_clip_get, host_modified,
                                host_mods };

/* A file the OS handed us - an argument, a double click, "Open With" - made a
 * (volume, name) the app can load, exactly as a picked file is. */
static void open_path(const char *path)
{
    char abs[1024], name[256];
    int vol;
    if (!uodesk_fs_abs(path, abs, (int)sizeof abs)) {
        fprintf(stderr, "%s: no such file\n", path);
        return;
    }
    vol = uodesk_fs_volume_of(abs, name, (int)sizeof name);
    if (vol < 0) { fprintf(stderr, "%s: cannot open that folder\n", abs); return; }
    uoa_host_open(vol, name);
}

/* ---- keyboard: plat events -> the (uni, scan, ctrl) the modules take ----- *
 * The modules speak UEFI's EFI_INPUT_KEY: a Unicode char, or 0 plus a scan
 * code for the keys that have no character.  Delete is a scan code with uni
 * 0 there and is reproduced exactly rather than given the "obvious" 127;
 * Esc is the one exception (see key_event). */
static int efi_scan(int k)
{
    switch (k) {
    case PK_UP: return 0x01;    case PK_DOWN: return 0x02;
    case PK_RIGHT: return 0x03; case PK_LEFT: return 0x04;
    case PK_HOME: return 0x05;  case PK_END: return 0x06;
    case PK_INSERT: return 0x07; case PK_DELETE: return 0x08;
    case PK_PGUP: return 0x09;  case PK_PGDN: return 0x0A;
    case PK_ESC: return 0x17;
    default: break;
    }
    if (k >= PK_F1 && k < PK_F1 + 10) return 0x0B + (k - PK_F1);
    if (k == PK_F1 + 10) return 0x15;
    if (k == PK_F1 + 11) return 0x16;
    return 0;
}

/* Command on a Mac is what Ctrl is everywhere else: the apps' accelerator
 * tables are Ctrl+letter, and a Mac user presses Cmd+S. */
static int is_accel(int mods)
{
#ifdef __APPLE__
    return (mods & (PM_CTRL | PM_GUI)) != 0;
#else
    /* Ctrl+Alt is AltGr on European layouts: that types characters */
    return (mods & PM_CTRL) && !(mods & PM_ALT);
#endif
}

static void feed(const unoui_event *ev)
{
    unoui_action a = unoui_handle(&UI, ev);
    if (a.changed && g_app->action) g_app->action(&a);
    g_dirty = 1;
}

/* pc64_uui.c's rule, reproduced: the module gets the key first, and what it
 * does not take is translated to a unoui event for the focused canvas. */
static void deliver_key(int uni, int scan, int ctrl, int mods)
{
    unoui_event ev;
    int vk = 0;
    g_key_mods = mods;
    if (g_app->key && g_app->key(uni, scan, ctrl)) { g_key_mods = 0; g_dirty = 1; return; }
    g_key_mods = 0;
    switch (scan) {
    case 0x01: vk = UI_KEY_UP; break;    case 0x02: vk = UI_KEY_DOWN; break;
    case 0x03: vk = UI_KEY_RIGHT; break; case 0x04: vk = UI_KEY_LEFT; break;
    case 0x05: vk = UI_KEY_HOME; break;  case 0x06: vk = UI_KEY_END; break;
    case 0x09: vk = UI_KEY_PGUP; break;  case 0x0A: vk = UI_KEY_PGDN; break;
    case 0x08: vk = UI_KEY_DELETE; break; case 0x17: vk = UI_KEY_ESC; break;
    }
    if (!vk) {
        if (uni == 0x0D || uni == 0x0A) vk = UI_KEY_ENTER;
        else if (uni == 0x08) vk = UI_KEY_BACKSPACE;
        else if (uni == 0x09) vk = UI_KEY_TAB;
    }
    memset(&ev, 0, sizeof ev);
    ev.mods = mods;
    if (vk) { ev.kind = UI_EV_KEY; ev.key = vk; feed(&ev); }
    else if (uni >= 32 && uni < 127) { ev.kind = UI_EV_CHAR; ev.ch = uni; feed(&ev); }
}

static void key_event(const plat_event *e)
{
    int accel = is_accel(e->mods), scan = efi_scan(e->key);

    /* F11 / Alt+Enter: a desktop convenience the OS window lacks otherwise.
     * Not while the show runs - its own exit path must stay the only one. */
    if ((e->key == PK_F1 + 10 || (e->key == PK_ENTER && (e->mods & PM_ALT))) &&
        !g_show_full) {
        plat_set_fullscreen(!plat_is_fullscreen());
        return;
    }
    /* Esc carries BOTH spellings.  The apps test `uni == 27` (ending the
     * slide show, leaving a cell or text-box edit) while hid_kbd.c reports
     * it as scan 0x17 alone; the unoui fallback keys off the scan.  27 is
     * below ' ', so no app can insert it as text. */
    if (e->key == PK_ESC) { deliver_key(27, scan, accel, e->mods); return; }
    if (scan) { deliver_key(0, scan, accel, e->mods); return; }
    switch (e->key) {
    case PK_ENTER:     deliver_key(0x0D, 0, accel, e->mods); return;
    case PK_BACKSPACE: deliver_key(0x08, 0, accel, e->mods); return;
    case PK_TAB:       deliver_key(0x09, 0, accel, e->mods); return;
    case PK_CHAR:
        /* Accelerators arrive as keys, never as PE_TEXT.  The modules accept
         * the letter with ctrl set (or its control code). */
        if (accel && e->ch >= 32 && e->ch < 127) deliver_key(e->ch, 0, 1, e->mods);
        return;
    default: return;
    }
}

static void text_event(const plat_event *e)
{
    const unsigned char *s = (const unsigned char *)e->text;
    /* The documents are byte strings in a single-byte model (unodoc), so
     * only ASCII goes in; a multi-byte UTF-8 sequence would be inserted as
     * its raw bytes.  Latin-1 and beyond is a model question for uoword.h,
     * not something the shell can paper over. */
    for (; *s; s++)
        if (*s >= 32 && *s < 127) deliver_key(*s, 0, 0, e->mods);
}

/* ---- the frame ------------------------------------------------------------ */
static void resize(int w, int h)
{
    if (w < 320) w = 320;
    if (h < 240) h = 240;
    if (w > FB_MAX_W) w = FB_MAX_W;
    if (h > FB_MAX_H) h = FB_MAX_H;
    uno_fb_w = w; uno_fb_h = h;
    UI.screen_w = w; UI.screen_h = h;
    UI.work.x = 0; UI.work.y = 0; UI.work.w = w; UI.work.h = h;
    g_win.r.x = 0; g_win.r.y = 0; g_win.r.w = w; g_win.r.h = h;
    fb_reset_clip();
    g_dirty = 1;
}

/* The app names its document in its unoui window title ("UnoWord -
 * Document1"); the OS title bar is where a desktop user looks for it. */
static void sync_title(void)
{
    const char *t = g_win.title ? g_win.title : g_app->name;
    if (strcmp(t, g_title)) {
        snprintf(g_title, sizeof g_title, "%s", t);
        plat_set_title(g_title);
    }
}

static void dispatch(const plat_event *e, int *running)
{
    unoui_event ev;
    memset(&ev, 0, sizeof ev);
    switch (e->type) {
    /* The close box asks; the app answers.  With unsaved changes it puts up
     * "Do you want to save...?" and quits through host_quit once that is
     * settled, so nothing here ends the program. */
    case PE_QUIT: if (uoa_host_close()) *running = 0; break;
    case PE_RESIZE: resize(e->w, e->h); break;
    case PE_EXPOSE: g_dirty = 1; break;
    case PE_MOUSE_MOVE:
        g_mx = e->x; g_my = e->y;
        ev.kind = UI_EV_MOUSE_MOVE; ev.x = e->x; ev.y = e->y; ev.mods = e->mods;
        feed(&ev); break;
    case PE_MOUSE_DOWN: case PE_MOUSE_UP:
        g_mx = e->x; g_my = e->y;
        ev.kind = e->type == PE_MOUSE_DOWN ? UI_EV_MOUSE_DOWN : UI_EV_MOUSE_UP;
        ev.x = e->x; ev.y = e->y; ev.button = e->button; ev.mods = e->mods;
        feed(&ev); break;
    case PE_WHEEL:
        if (!e->wheel) break;
        ev.kind = UI_EV_WHEEL; ev.x = g_mx; ev.y = g_my; ev.wheel = e->wheel;
        feed(&ev); break;
    case PE_KEY:  key_event(e); break;
    case PE_TEXT: text_event(e); break;
    default: break;
    }
    /* A native file dialog ran modally inside that event.  The app learns
     * the dialog closed on the NEXT event it sees (uofile.h), so give it
     * one: a pointer move to where the pointer already is. */
    if (g_kick) {
        g_kick = 0;
        memset(&ev, 0, sizeof ev);
        ev.kind = UI_EV_MOUSE_MOVE; ev.x = g_mx; ev.y = g_my;
        feed(&ev);
    }
}

/* ---- --script: replay input, for the smoke test ---------------------------
 * One command per line, run one per loop turn so each command's events have
 * gone through the real dispatch path before the next:
 *
 *   text Hello world      typed text
 *   key F5 | key s ctrl   a key by name, optional ctrl/shift/alt/gui
 *   click 120 40 [shift]  left press + release at x, y
 *   shot out.ppm          render, write the frame
 *   frames 30             let 30 ticks pass (caret, transitions)
 *   sleep 500             let 500 ms of real time pass (an OS event)
 *   pick /path/file.doc   what the next File > Open / Save As dialog
 *                         "chooses" (the OS picker cannot be scripted)
 *   open /path/file.doc   the OS handing us a file (a double click)
 *   quit                  the window's close box
 *   expect title TEXT     the window title must read TEXT
 *   expect dirty 0|1      whether the app reports unsaved changes
 *   clip TEXT             put TEXT on the OS clipboard, as another program
 *                         would ("\t" and "\n" spell a tab and a line end)
 *   expect clip TEXT      the OS clipboard must hold TEXT (same spelling)
 *   passed FILE           write FILE if every expect so far has held
 *   # ...                 a comment
 *
 * The script ends the program when it runs out. */
static FILE *g_script;
static int   g_script_fail;          /* an `expect` did not hold: exit 1 */
static int   g_wait_frames, g_pending_shot;
static unsigned g_sleep_until;       /* `sleep`: real time, for OS events */
static char  g_shot_path[512];
static char  g_pick[1024];
static plat_event g_q[128];
static int   g_qh, g_qt;

static void q_push(const plat_event *e)
{
    int n = (g_qt + 1) % 128;
    if (n != g_qh) { g_q[g_qt] = *e; g_qt = n; }
}

static int key_by_name(const char *n, int *ch)
{
    static const struct { const char *n; int k; } t[] = {
        { "Up", PK_UP }, { "Down", PK_DOWN }, { "Right", PK_RIGHT }, { "Left", PK_LEFT },
        { "Home", PK_HOME }, { "End", PK_END }, { "Insert", PK_INSERT },
        { "Delete", PK_DELETE }, { "PageUp", PK_PGUP }, { "PageDown", PK_PGDN },
        { "Escape", PK_ESC }, { "Return", PK_ENTER }, { "Backspace", PK_BACKSPACE },
        { "Tab", PK_TAB }
    };
    unsigned i;
    *ch = 0;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(n, t[i].n)) return t[i].k;
    if (n[0] == 'F' && n[1] >= '1' && n[1] <= '9') {
        int f = atoi(n + 1);
        if (f >= 1 && f <= 12) return PK_F1 + f - 1;
    }
    if (n[0] && !n[1]) { *ch = n[0] >= 'A' && n[0] <= 'Z' ? n[0] + 32 : n[0]; return PK_CHAR; }
    return PK_NONE;
}

static void push_key(const char *name, const char *mods)
{
    plat_event e;
    memset(&e, 0, sizeof e);
    e.type = PE_KEY;
    e.key = key_by_name(name, &e.ch);
    if (e.key == PK_NONE) { fprintf(stderr, "script: unknown key '%s'\n", name); return; }
    if (mods && strstr(mods, "ctrl"))  e.mods |= PM_CTRL;
    if (mods && strstr(mods, "shift")) e.mods |= PM_SHIFT;
    if (mods && strstr(mods, "alt"))   e.mods |= PM_ALT;
    if (mods && strstr(mods, "gui"))   e.mods |= PM_GUI;
    /* a plain character key with no accelerator is typing, not a PE_KEY */
    if (e.key == PK_CHAR && !is_accel(e.mods)) {
        e.type = PE_TEXT; e.text[0] = (char)e.ch; e.text[1] = 0;
    }
    q_push(&e);
}

static void push_click(int x, int y, int mods)
{
    plat_event e;
    memset(&e, 0, sizeof e);
    e.x = x; e.y = y; e.mods = mods;
    e.type = PE_MOUSE_MOVE; q_push(&e);
    e.type = PE_MOUSE_DOWN; q_push(&e);
    e.type = PE_MOUSE_UP;   q_push(&e);
}

/* the scripted stand-in for the OS picker: `pick` names the answer */
static int script_picker(int save, const char *const *types, int ntypes,
                         int *vol, char *name, int cap, int *type)
{
    (void)save;
    if (!g_pick[0]) { g_kick = 1; return 0; }
    *type = plat_type_of_path(g_pick, types, ntypes);
    *vol = uodesk_fs_volume_of(g_pick, name, cap);
    g_pick[0] = 0;
    g_kick = 1;
    return *vol >= 0;
}

/* "a\tb\n" in a script line -> a real tab and line end */
static void unescape(const char *s, char *out, int cap)
{
    int k = 0;
    for (; *s && k < cap - 1; s++) {
        if (s[0] == '\\' && s[1] == 't') { out[k++] = '\t'; s++; }
        else if (s[0] == '\\' && s[1] == 'n') { out[k++] = '\n'; s++; }
        else out[k++] = *s;
    }
    out[k] = 0;
}

/* 0 = the script is finished */
static int script_step(void)
{
    char line[1024], a[256], b[64];
    int x, y, n;
    if (g_wait_frames > 0) { g_wait_frames--; return 1; }
    if (g_sleep_until && (int)(plat_ticks() - g_sleep_until) < 0) return 1;
    g_sleep_until = 0;
    if (!fgets(line, sizeof line, g_script)) return 0;
    n = (int)strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n || line[0] == '#') return 1;
    if (!strncmp(line, "text ", 5)) {
        const char *s = line + 5;
        for (; *s; s++) {
            plat_event e;
            memset(&e, 0, sizeof e);
            e.type = PE_TEXT; e.text[0] = *s;
            q_push(&e);
        }
    } else if (!strncmp(line, "pick ", 5)) snprintf(g_pick, sizeof g_pick, "%s", line + 5);
    /* "open PATH": the OS handing us a file (a double click); "quit": the
     * window's close box.  The two things a script cannot otherwise do. */
    else if (!strncmp(line, "open ", 5)) plat_post_open(line + 5);
    else if (!strncmp(line, "passed ", 7)) {
        /* proof for a harness that cannot see our exit code (macOS `open`) */
        FILE *f = g_script_fail ? 0 : fopen(line + 7, "w");
        if (f) { fputs("ok\n", f); fclose(f); }
    }
    else if (!strncmp(line, "expect title ", 13)) {
        const char *t = g_win.title ? g_win.title : "";
        if (strcmp(t, line + 13)) {
            fprintf(stderr, "script: FAIL: title is '%s', expected '%s'\n", t, line + 13);
            g_script_fail = 1;
        }
    } else if (sscanf(line, "expect dirty %d", &x) == 1) {
        if (!!uoa_host_dirty() != !!x) {
            fprintf(stderr, "script: FAIL: dirty is %d, expected %d\n", uoa_host_dirty(), x);
            g_script_fail = 1;
        }
    }
    else if (!strncmp(line, "clip ", 5)) {
        char t[1024];
        unescape(line + 5, t, (int)sizeof t);
        plat_clip_set(t);
    } else if (!strncmp(line, "expect clip ", 12)) {
        char want[1024], got[1024];
        unescape(line + 12, want, (int)sizeof want);
        plat_clip_get(got, (long)sizeof got);
        if (strcmp(got, want)) {
            fprintf(stderr, "script: FAIL: clipboard is '%s', expected '%s'\n", got, want);
            g_script_fail = 1;
        }
    }
    else if (!strcmp(line, "quit")) {
        plat_event e;
        memset(&e, 0, sizeof e);
        e.type = PE_QUIT;
        q_push(&e);
    }
    else if (sscanf(line, "key %255s %63s", a, b) == 2) push_key(a, b);
    else if (sscanf(line, "key %255s", a) == 1) push_key(a, 0);
    else if (sscanf(line, "click %d %d", &x, &y) == 2)
        push_click(x, y, strstr(line, "shift") ? PM_SHIFT : 0);
    else if (sscanf(line, "frames %d", &x) == 1) g_wait_frames = x;
    else if (sscanf(line, "sleep %d", &x) == 1) g_sleep_until = plat_ticks() + (unsigned)x;
    else if (sscanf(line, "shot %511s", g_shot_path) == 1) { g_pending_shot = 1; g_dirty = 1; }
    else fprintf(stderr, "script: cannot parse '%s'\n", line);
    return 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options] [FILE]\n"
        "  FILE     a document to open\n"
        "  --shot   render one frame to a PPM and exit (headless check)\n"
        "  --script replay input from FILE, then exit (see uodesk.c)\n"
        "  --size   initial window size (default 1024x720)\n"
        "  --dir    a folder to show in the drawn Open/Save dialog\n"
        "           (repeatable; default: Documents, Desktop and home)\n", argv0);
}

int main(int argc, char **argv)
{
    int i;
    const char *shot = 0;
    int w0 = 1024, h0 = 720;
    unsigned last_tick = 0;

    plat_args(&argc, &argv);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
            if (!(g_script = fopen(argv[++i], "r"))) { perror(argv[i]); return 2; }
        }
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &w0, &h0) != 2) { usage(argv[0]); return 2; }
        }
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) uodesk_fs_add_dir(argv[++i], 0);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        /* macOS passes -psn_* to apps launched from Finder on old systems */
        else if (!strncmp(argv[i], "-psn_", 5)) continue;
        else if (argv[i][0] == '-' && argv[i][1]) { usage(argv[0]); return 2; }
        /* a document: Explorer's "%1", the .desktop entry's %F.  One window
         * holds one document, so the first is opened and the rest queue
         * behind it, which is what a Finder multi-select also does. */
        else plat_post_open(argv[i]);
    }

    g_app = uno_app_main(0);
    if (!plat_init(g_app->name, w0, h0, shot || g_script)) return 1;
    uodesk_fs_init(plat_base_path());
    uof_set_native(g_script ? script_picker : native_picker);
    uoa_set_host(&kHost);

    /* the pc64 shell's boot order (pc64_uui.c): font BEFORE anything is laid
     * out, because every metric the chrome computes comes from it */
    uno_font_set_subpixel(0);   /* LCD AA assumes RGB stripes the OS may not */
    uno_font_use(0);
    unoui_ui_init(&UI, &theme_aurora_light, w0, h0);
    resize(w0, h0);

    g_app->build(&g_win);
    if (g_app->opened) g_app->opened();
    unoui_ui_add(&UI, &g_win);
    resize(w0, h0);             /* build() sized the window for a desktop   */
    unoui_fullscreen(&UI, &g_win);
    if (g_app->canvas_index) {
        int wi = g_app->canvas_index();
        if (wi >= 0) { UI.focus_win = 0; UI.focus_wi = wi; }
    }

    while (g_running) {
        plat_event e;
        unsigned now;
        char path[1024];

        if (g_qh != g_qt) {                         /* scripted input first */
            e = g_q[g_qh]; g_qh = (g_qh + 1) % 128;
            dispatch(&e, &g_running);
        }
        while (plat_wait(&e, ((shot || g_script) && !g_sleep_until) || g_qh != g_qt ? 0 : 16))
            dispatch(&e, &g_running);
        /* a file the OS handed over; one per turn, so each open (and any
         * "save changes?" it raises) has settled before the next */
        if (plat_take_open(path, (int)sizeof path)) {
            unoui_event k;
            open_path(path);
            memset(&k, 0, sizeof k);
            k.kind = UI_EV_MOUSE_MOVE; k.x = g_mx; k.y = g_my;
            g_kick = 0;
            feed(&k);
        }
        uoa_host_tick();

        /* caret blink + the module's per-frame hook, at the pc64 cadence */
        now = plat_ticks();
        if (now - last_tick >= 16 || g_script) {
            unoui_event t;
            last_tick = now;
            memset(&t, 0, sizeof t);
            t.kind = UI_EV_TICK;
            unoui_handle(&UI, &t);
            if (g_app->frame) g_app->frame();
            if ((UI.ticks & 15) == 0) g_dirty = 1;
        }

        if (g_script && g_qh == g_qt && !g_pending_shot && !script_step()) g_running = 0;

        if (!g_dirty && !shot) continue;
        g_dirty = 0;
        fb_reset_clip();
        unoui_render_ui(&UI);
        sync_title();

        if (g_pending_shot) {
            g_pending_shot = 0;
            if (!uodesk_write_ppm(g_shot_path, fb, FB_W, FB_H))
                fprintf(stderr, "script: cannot write %s\n", g_shot_path);
        }
        if (shot) {
            int ok = uodesk_write_ppm(shot, fb, FB_W, FB_H);
            if (g_app->closed) g_app->closed();
            plat_shutdown();
            return ok ? 0 : 1;
        }
        plat_present(fb, FB_W, FB_H);
    }

    if (g_app->closed) g_app->closed();
    plat_shutdown();
    return g_script_fail ? 1 : 0;
}

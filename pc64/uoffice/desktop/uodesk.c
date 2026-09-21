/* ===========================================================================
 * uodesk.c - UnoWord / UnoCalc / UnoShow as native Windows, macOS and Linux
 * applications.                                            [EXPERIMENTAL]
 *
 * This is a SHELL, not a port of the apps.  pc64/apps/uo*.c are compiled
 * unmodified, exactly as build.sh compiles them into APPS\UO*.UNO, and they
 * are hosted through the same UnoUuiApp vtable pc64_uui.c drives.  What this
 * file supplies is what the pc64 shell supplies: a framebuffer, the event
 * pump, and the handful of pc64_shell_* services a module imports by name.
 *
 * The window IS the frame.  pc64 puts a module in a unoui window with a title
 * bar; on a desktop OS the OS already drew one, so the app's window is held
 * in unoui's FULLSCREEN mode permanently - its canvas owns the whole
 * framebuffer and every event, which is the path UnoShow's slide show and
 * every native game already exercise on pc64.  Resizing the OS window is then
 * just a new screen size.
 *
 * Pixels are the SAME pixels: pc64/fb.c renders into the software
 * framebuffer, and all this file does is hand that buffer to SDL once a frame
 * (fb_px is 0xAABBGGRR, i.e. R,G,B,A in memory = SDL_PIXELFORMAT_ABGR8888).
 * ======================================================================== */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uno_uuiapp.h"
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "pc64_font.h"
#include "uodesk.h"

const UnoUuiApp *uno_app_main(void *reserved);   /* the app this binary is */

/* ---- the framebuffer size (pc64: uefi_main.c owns these) ---------------- */
int uno_fb_w = 1024, uno_fb_h = 720;

static unoui_ui      UI;
static unoui_window  g_win;
static const UnoUuiApp *g_app;
static SDL_Window   *g_sdlwin;
static int           g_dirty = 1;
static int           g_osfull;    /* the OS window is fullscreen (slide show) */

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
    g_osfull = w != 0;
    SDL_SetWindowFullscreen(g_sdlwin, g_osfull ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    g_dirty = 1;
}
int pc64_shell_is_fullscreen(void) { return g_osfull; }

/* ---- keyboard: SDL -> the (uni, scan, ctrl) the modules take ------------ *
 * The modules speak UEFI's EFI_INPUT_KEY: a Unicode char, or 0 plus a scan
 * code for the keys that have no character.  Delete is a scan code with uni
 * 0 there and is reproduced exactly rather than given the "obvious" 127;
 * Esc is the one exception (see on_keydown). */
static int efi_scan(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: return 0x01;    case SDLK_DOWN: return 0x02;
    case SDLK_RIGHT: return 0x03; case SDLK_LEFT: return 0x04;
    case SDLK_HOME: return 0x05;  case SDLK_END: return 0x06;
    case SDLK_INSERT: return 0x07; case SDLK_DELETE: return 0x08;
    case SDLK_PAGEUP: return 0x09; case SDLK_PAGEDOWN: return 0x0A;
    case SDLK_ESCAPE: return 0x17;
    default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F10) return 0x0B + (int)(k - SDLK_F1);
    if (k == SDLK_F11) return 0x15;
    if (k == SDLK_F12) return 0x16;
    return 0;
}

static int ui_mods(Uint16 m)
{
    int r = 0;
    if (m & KMOD_SHIFT) r |= UI_MOD_SHIFT;
    if (m & KMOD_CTRL)  r |= UI_MOD_CTRL;
    if (m & KMOD_ALT)   r |= UI_MOD_ALT;
    if (m & KMOD_GUI)   r |= UI_MOD_GUI;
    return r;
}

/* Command on a Mac is what Ctrl is everywhere else: the apps' accelerator
 * tables are Ctrl+letter, and a Mac user presses Cmd+S. */
static int is_accel(Uint16 m)
{
#ifdef __APPLE__
    return (m & (KMOD_CTRL | KMOD_GUI)) != 0;
#else
    return (m & KMOD_CTRL) != 0;
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
    if (g_app->key && g_app->key(uni, scan, ctrl)) { g_dirty = 1; return; }
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

static void on_keydown(const SDL_KeyboardEvent *k)
{
    SDL_Keycode sym = k->keysym.sym;
    Uint16 m = k->keysym.mod;
    int mods = ui_mods(m), scan = efi_scan(sym);

    /* F11 / Alt+Enter: a desktop convenience the OS window lacks otherwise.
     * Not while the show runs - its own exit path must stay the only one. */
    if ((sym == SDLK_F11 || (sym == SDLK_RETURN && (m & KMOD_ALT))) && !g_osfull) {
        Uint32 f = SDL_GetWindowFlags(g_sdlwin);
        SDL_SetWindowFullscreen(g_sdlwin, (f & SDL_WINDOW_FULLSCREEN_DESKTOP)
                                          ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        return;
    }
    /* Esc carries BOTH spellings.  The apps test `uni == 27` (ending the
     * slide show, leaving a cell or text-box edit) while hid_kbd.c reports
     * it as scan 0x17 alone; the unoui fallback keys off the scan.  27 is
     * below ' ', so no app can insert it as text. */
    if (sym == SDLK_ESCAPE) { deliver_key(27, scan, is_accel(m), mods); return; }
    if (scan) { deliver_key(0, scan, is_accel(m), mods); return; }
    switch (sym) {
    case SDLK_RETURN: case SDLK_KP_ENTER: deliver_key(0x0D, 0, is_accel(m), mods); return;
    case SDLK_BACKSPACE: deliver_key(0x08, 0, is_accel(m), mods); return;
    case SDLK_TAB:       deliver_key(0x09, 0, is_accel(m), mods); return;
    default: break;
    }
    /* Accelerators arrive here, NOT as SDL_TEXTINPUT (which is suppressed
     * while an accelerator modifier is down - see the pump).  The modules
     * accept either the letter or its control code with ctrl set. */
    if (is_accel(m) && sym >= 32 && sym < 127) {
        int c = (int)sym;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        deliver_key(c, 0, 1, mods);
    }
}

static void on_text(const char *s, Uint16 m)
{
    if (is_accel(SDL_GetModState() | m)) return;
    /* The documents are byte strings in a single-byte model (unodoc), so
     * only ASCII goes in; a multi-byte UTF-8 sequence would be inserted as
     * its raw bytes.  Latin-1 and beyond is a model question for uoword.h,
     * not something the shell can paper over. */
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 32 && c < 127) deliver_key(c, 0, 0, ui_mods(m));
    }
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

/* ---- --script: replay input, for the smoke test ---------------------------
 * One command per line, run one per loop turn so each command's events have
 * gone through the real pump (on_keydown / on_text / feed) before the next:
 *
 *   text Hello world      typed text (SDL_TEXTINPUT)
 *   key F5 | key s ctrl   a key by SDL name, optional ctrl/shift/alt/gui
 *   click 120 40          left press + release at x, y
 *   shot out.ppm          render, write the frame
 *   frames 30             let 30 ticks pass (caret, transitions)
 *   size 800x600          resize the window
 *   # ...                 a comment
 *
 * Every pushed event carries the window's ID: sdl2-compat (what Homebrew
 * now ships as "sdl2": SDL2's API over SDL3) looks the window up while
 * pushing a text event and faults on ID 0.
 *
 * The script ends the program when it runs out.  The events are pushed into
 * SDL's own queue rather than called in directly, so what is tested is the
 * path a real keyboard takes, not a shortcut beside it. */
static FILE *g_script;
static int   g_wait_frames, g_pending_shot;
static char  g_shot_path[512];

static void push_key(const char *name, const char *mods)
{
    SDL_Event e;
    SDL_Keycode k = SDL_GetKeyFromName(name);
    Uint16 m = 0;
    if (k == SDLK_UNKNOWN) { fprintf(stderr, "script: unknown key '%s'\n", name); return; }
    if (mods && strstr(mods, "ctrl"))  m |= KMOD_LCTRL;
    if (mods && strstr(mods, "shift")) m |= KMOD_LSHIFT;
    if (mods && strstr(mods, "alt"))   m |= KMOD_LALT;
    if (mods && strstr(mods, "gui"))   m |= KMOD_LGUI;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN; e.key.state = SDL_PRESSED;
    e.key.keysym.sym = k; e.key.keysym.scancode = SDL_GetScancodeFromKey(k);
    e.key.keysym.mod = m;
    e.key.windowID = SDL_GetWindowID(g_sdlwin);
    SDL_PushEvent(&e);
    e.type = SDL_KEYUP; e.key.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

static void push_click(int x, int y)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEMOTION; e.motion.x = x; e.motion.y = y;
    e.motion.windowID = SDL_GetWindowID(g_sdlwin);
    SDL_PushEvent(&e);
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_LEFT;
    e.button.state = SDL_PRESSED; e.button.clicks = 1; e.button.x = x; e.button.y = y;
    e.button.windowID = SDL_GetWindowID(g_sdlwin);
    SDL_PushEvent(&e);
    e.type = SDL_MOUSEBUTTONUP; e.button.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

/* 0 = the script is finished */
static int script_step(void)
{
    char line[512], a[256], b[64];
    int x, y, n;
    if (g_wait_frames > 0) { g_wait_frames--; return 1; }
    if (!fgets(line, sizeof line, g_script)) return 0;
    n = (int)strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n || line[0] == '#') return 1;
    if (!strncmp(line, "text ", 5)) {
        const char *s = line + 5;
        for (; *s; s++) {
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = SDL_TEXTINPUT; e.text.text[0] = *s;
            e.text.windowID = SDL_GetWindowID(g_sdlwin);
            SDL_PushEvent(&e);
        }
    } else if (sscanf(line, "key %255s %63s", a, b) == 2) push_key(a, b);
    else if (sscanf(line, "key %255s", a) == 1) push_key(a, 0);
    else if (sscanf(line, "click %d %d", &x, &y) == 2) push_click(x, y);
    else if (sscanf(line, "frames %d", &x) == 1) g_wait_frames = x;
    else if (sscanf(line, "size %dx%d", &x, &y) == 2) SDL_SetWindowSize(g_sdlwin, x, y);
    else if (sscanf(line, "shot %511s", g_shot_path) == 1) { g_pending_shot = 1; g_dirty = 1; }
    else fprintf(stderr, "script: cannot parse '%s'\n", line);
    return 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--shot out.ppm] [--script FILE] [--size WxH] [--dir PATH]...\n"
        "  --shot   render one frame to a PPM and exit (headless check)\n"
        "  --script replay input from FILE, then exit (see uodesk.c)\n"
        "  --size   initial window size (default 1024x720)\n"
        "  --dir    a folder to show in Open/Save (repeatable; default:\n"
        "           Documents, Desktop and home)\n", argv0);
}

int main(int argc, char **argv)
{
    SDL_Renderer *ren;
    SDL_Texture  *tex = 0;
    int tex_w = 0, tex_h = 0, running = 1, i;
    const char *shot = 0;
    int w0 = 1024, h0 = 720;
    Uint32 last_tick = 0;

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
        else { usage(argv[0]); return 2; }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    uodesk_fs_init(SDL_GetBasePath());

    g_app = uno_app_main(0);
    g_sdlwin = SDL_CreateWindow(g_app->name, SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, w0, h0,
                                SDL_WINDOW_RESIZABLE |
                                (shot || g_script ? SDL_WINDOW_HIDDEN : 0));
    if (!g_sdlwin) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(g_sdlwin, 480, 320);
    ren = SDL_CreateRenderer(g_sdlwin, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(g_sdlwin, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }

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
    SDL_StartTextInput();

    while (running) {
        SDL_Event e;
        Uint32 now;
        int got = shot ? 0 : SDL_WaitEventTimeout(&e, 16);
        while (got) {
            unoui_event ev;
            memset(&ev, 0, sizeof ev);
            switch (e.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                    resize(e.window.data1, e.window.data2);
                else if (e.window.event == SDL_WINDOWEVENT_EXPOSED)
                    g_dirty = 1;
                break;
            case SDL_MOUSEMOTION:
                ev.kind = UI_EV_MOUSE_MOVE; ev.x = e.motion.x; ev.y = e.motion.y;
                ev.mods = ui_mods(SDL_GetModState());
                feed(&ev); break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                ev.kind = e.type == SDL_MOUSEBUTTONDOWN ? UI_EV_MOUSE_DOWN : UI_EV_MOUSE_UP;
                ev.x = e.button.x; ev.y = e.button.y;
                ev.button = e.button.button == SDL_BUTTON_LEFT ? 0
                          : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
                ev.mods = ui_mods(SDL_GetModState());
                feed(&ev); break;
            case SDL_MOUSEWHEEL: {
                int mx, my, n = e.wheel.y;
                if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) n = -n;
                if (!n) break;
                SDL_GetMouseState(&mx, &my);
                ev.kind = UI_EV_WHEEL; ev.x = mx; ev.y = my;
                ev.wheel = -n;          /* unoui: + is down, SDL: + is up */
                feed(&ev); break;
            }
            case SDL_KEYDOWN: on_keydown(&e.key); break;
            case SDL_TEXTINPUT: on_text(e.text.text, SDL_GetModState()); break;
            default: break;
            }
            got = SDL_PollEvent(&e);
        }

        /* caret blink + the module's per-frame hook, at the pc64 cadence */
        now = SDL_GetTicks();
        if (now - last_tick >= 16) {
            unoui_event t;
            last_tick = now;
            memset(&t, 0, sizeof t);
            t.kind = UI_EV_TICK;
            unoui_handle(&UI, &t);
            if (g_app->frame) g_app->frame();
            if ((UI.ticks & 15) == 0) g_dirty = 1;
        }

        if (g_script && !g_pending_shot && !script_step()) running = 0;

        if (!g_dirty && !shot) continue;
        g_dirty = 0;
        fb_reset_clip();
        unoui_render_ui(&UI);

        if (g_pending_shot) {
            g_pending_shot = 0;
            if (!uodesk_write_ppm(g_shot_path, fb, FB_W, FB_H))
                fprintf(stderr, "script: cannot write %s\n", g_shot_path);
        }
        if (shot) {
            int ok = uodesk_write_ppm(shot, fb, FB_W, FB_H);
            if (g_app->closed) g_app->closed();
            SDL_Quit();
            return ok ? 0 : 1;
        }
        if (!tex || tex_w != FB_W || tex_h != FB_H) {
            if (tex) SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                    SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
            tex_w = FB_W; tex_h = FB_H;
        }
        SDL_UpdateTexture(tex, 0, fb, FB_W * (int)sizeof(fb_px));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, 0, 0);
        SDL_RenderPresent(ren);
    }

    if (g_app->closed) g_app->closed();
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(g_sdlwin);
    SDL_Quit();
    return 0;
}

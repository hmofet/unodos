/* ===========================================================================
 * UnoDOS/pc64 - the LEGACY-APP BRIDGE for the unoui shell.
 *
 * The family's apps (games, paint, tracker, music, ...) are self-contained
 * AppInterface modules that draw their content into a window's bounds through
 * a KernelApi of fb-backed primitives. They carry real game/tool logic worth
 * keeping. Rather than rewrite each against unoui widgets, this bridge hosts
 * them UNCHANGED as unoui canvases: unoui owns the window/desktop/taskbar/drag,
 * and each app just paints into its canvas rect and receives mapped input.
 *
 * It supplies its own compact KernelApi (drawing + util copied from unodos.c's
 * colour path; sound + FS already live in pc64_io; the window-manager hooks are
 * redirected to "mark the shell dirty"), so NONE of unodos.c's window manager
 * is linked. Toolbox primitives come from mac_compat.c.
 * ======================================================================== */
#include "uno_app.h"        /* AppInterface, KernelApi, mac_compat.h (Toolbox) */
#include "unoui.h"
#include "mac_compat.h"     /* uno_pc64_res_* */
#include "unosound.h"       /* the shared single-voice audio (Music/Tracker/games) */
#include <string.h>

enum { C_BLUE = 0, C_CYAN = 1, C_MAG = 2, C_WHITE = 3 };

/* ---- palette + drawing primitives (unodos.c colour path, verbatim) ------- */
static RGBColor kPalette[4] = {
    { 0x0000, 0x0000, 0xAAAA }, { 0x0000, 0xAAAA, 0xAAAA },
    { 0xAAAA, 0x0000, 0xAAAA }, { 0xFFFF, 0xFFFF, 0xFFFF }
};
static RGBColor kBlack = { 0, 0, 0 };
#define TBAR_H 18

static void uno_fill(Rect *r, short c)
{ RGBForeColor(&kPalette[c]); PaintRect(r); RGBForeColor(&kBlack); }
static void uno_box(Rect *r, short c)
{ RGBForeColor(&kPalette[c]); FrameRect(r); RGBForeColor(&kBlack); }
static void uno_invert(Rect *r) { InvertRect(r); }

static void text_at(short x, short y, const char *s, short fg, short bg, Boolean opaque)
{
    short len = (short)strlen(s);
    MoveTo(x, y);
    RGBForeColor(&kPalette[fg]);
    if (opaque) { RGBBackColor(&kPalette[bg]); TextMode(srcCopy); }
    else        { TextMode(srcOr); }
    DrawText((Ptr)s, 0, len);
    RGBForeColor(&kBlack); RGBBackColor(&kPalette[C_WHITE]); TextMode(srcOr);
}
static void text_at_max(short x, short y, const char *s, short fg, short maxw)
{
    short len = (short)strlen(s);
    while (len > 0 && TextWidth((Ptr)s, 0, len) > maxw) len--;
    if (len <= 0) return;
    MoveTo(x, y);
    RGBForeColor(&kPalette[fg]); TextMode(srcOr);
    DrawText((Ptr)s, 0, len); RGBForeColor(&kBlack);
}
static void fill_rgb(Rect *q, const GameRGB *c)
{
    RGBColor rc;
    rc.red = (unsigned short)(c->r << 8); rc.green = (unsigned short)(c->g << 8);
    rc.blue = (unsigned short)(c->b << 8);
    RGBForeColor(&rc); PaintRect(q); RGBForeColor(&kBlack);
}

/* ---- util ---------------------------------------------------------------- */
static long gBootTicks;
static long now_secs(void) { return (TickCount() - gBootTicks) / 60; }
static void fmt_u(long v, char *out)
{
    char tmp[12]; int n = 0, i = 0;
    if (v <= 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) out[i++] = tmp[--n];
    out[i] = 0;
}
static void put2(long v, char *out) { out[0]='0'+(char)((v/10)%10); out[1]='0'+(char)(v%10); out[2]=0; }

/* ---- sound: routed to UnoSound, the shared single-voice sequencer ---------
 * Music/Tracker reach these through the KernelApi; the native games use
 * UnoSound directly. All share the one PC-speaker voice, which the shell loop
 * advances via uno_seq_tick() - so the old per-app sequencer here is gone.
 * (Note and u_seqnote_t are both {u8 midi; u8 dur}, so a score casts across.) */
static void music_open_chan(void) { }                 /* UnoSound is inited by the shell */
static void music_note_on(short midi, short durTicks)
{ uno_seq_beep(midi, durTicks > 0 ? durTicks : 6); }  /* one-shot note */
static void music_quiet(void) { uno_seq_stop(); }
static void music_start(void) { }
static void music_stop(void)  { uno_seq_stop(); }

static short gActiveProc = -1;             /* focused app (used by kapi_topmost) */

static void gm_start(const Note *notes, short count, short owner)
{ (void)owner; uno_seq_play((const u_seqnote_t *)notes, count); }   /* loop a song */
static void gm_stop(void) { uno_seq_stop(); }
static void gm_tick(void) { }              /* UnoSound advances in the shell loop */

/* ---- window-manager hooks -> "mark the shell dirty" ---------------------- */
static int    *gDirty;
static UnoWin  gAppWins[APP_NAPPS];
static Boolean gAppLive[APP_NAPPS];
static UnoWin *find_app_window(short proc)
{ return (proc >= 0 && proc < APP_NAPPS && gAppLive[proc]) ? &gAppWins[proc] : 0; }
static void draw_window(UnoWin *w) { (void)w; if (gDirty) *gDirty = 1; }
static void repaint_all(void)      { if (gDirty) *gDirty = 1; }
static void launch_app(short proc) { (void)proc; }   /* the shell owns launching */
static short kapi_topmost(void)    { return gActiveProc; }

/* ---- files: Paint/Tracker load/save, wired to pc64_io's RAM-disk FS via the
 * Mac-Toolbox file calls (same path the Editor uses). ----------------------- */
static short         gFatCount;
static unsigned char gFatNames[16][13];
static long          gFatSizes[16];
static void bc2p(const char *s, unsigned char *p)   /* C string -> Pascal string */
{ int n = 0; while (s[n] && n < 31) n++; p[0] = (unsigned char)n;
  { int i; for (i = 0; i < n; i++) p[i + 1] = (unsigned char)s[i]; } }
static Boolean fat12_mount(void) { return true; }
static void    fat12_list(void)  { gFatCount = 0; }   /* name listing: TODO      */
static long    fat12_read(const char *name, unsigned char *buf, long max)
{
    unsigned char pn[34]; short ref; long cnt = max;
    bc2p(name, pn);
    if (FSOpen(pn, 0, &ref) != noErr) return -1;
    if (FSRead(ref, &cnt, buf) != noErr) cnt = -1;
    FSClose(ref);
    return cnt;
}
static Boolean fat12_write(const char *name, const unsigned char *buf, long len)
{
    unsigned char pn[34]; short ref; long cnt = len;
    bc2p(name, pn);
    Create(pn, 0, 0, 0);
    if (FSOpen(pn, 0, &ref) != noErr) return false;
    FSWrite(ref, &cnt, buf); FSClose(ref);
    return true;
}

/* ---- the KernelApi handed to every app ----------------------------------- */
static KernelApi gKApi;
static const AppInterface *gIface[APP_NAPPS];
static int                 gTried[APP_NAPPS];

/* entry symbols of the migrated apps (compiled with -DUNO_APP_SYM=...) */
extern const AppInterface *uno_app_main_dostris(const KernelApi *);
extern const AppInterface *uno_app_main_pacman (const KernelApi *);
extern const AppInterface *uno_app_main_outlast(const KernelApi *);
extern const AppInterface *uno_app_main_music  (const KernelApi *);
extern const AppInterface *uno_app_main_tracker(const KernelApi *);
extern const AppInterface *uno_app_main_paint  (const KernelApi *);
extern const AppInterface *uno_app_main_network(const KernelApi *);

static UnoAppEntry entry_for(short proc)
{
    switch (proc) {
    case APP_DOSTRIS: return uno_app_main_dostris;
    case APP_PACMAN:  return uno_app_main_pacman;
    case APP_OUTLAST: return uno_app_main_outlast;
    case APP_MUSIC:   return uno_app_main_music;
    case APP_TRACKER: return uno_app_main_tracker;
    case APP_PAINT:   return uno_app_main_paint;
    case APP_NETWORK: return uno_app_main_network;
    default:          return 0;
    }
}

void unoapp_init(int *dirtyflag)
{
    gDirty = dirtyflag; gBootTicks = TickCount();
    gKApi.abi_version = UNO_ABI_VERSION;
    gKApi.palette = kPalette; gKApi.black = &kBlack; gKApi.tbar_h = TBAR_H;
    gKApi.uno_fill = uno_fill; gKApi.uno_box = uno_box; gKApi.uno_invert = uno_invert;
    gKApi.text_at = text_at; gKApi.text_at_max = text_at_max; gKApi.fill_rgb = fill_rgb;
    gKApi.fmt_u = fmt_u; gKApi.put2 = put2; gKApi.now_secs = now_secs;
    gKApi.draw_window = draw_window; gKApi.find_app_window = find_app_window;
    gKApi.launch_app = launch_app; gKApi.repaint_all = repaint_all;
    gKApi.topmost_proc = kapi_topmost;
    gKApi.fat12_mount = fat12_mount; gKApi.fat12_list = fat12_list;
    gKApi.fat12_read = fat12_read; gKApi.fat12_write = fat12_write;
    gKApi.fat_count = &gFatCount; gKApi.fat_name = gFatNames; gKApi.fat_sizes = gFatSizes;
    gKApi.music_open_chan = music_open_chan; gKApi.music_note_on = music_note_on;
    gKApi.music_quiet = music_quiet; gKApi.music_start = music_start;
    gKApi.music_stop = music_stop; gKApi.gm_start = gm_start; gKApi.gm_stop = gm_stop;
    gKApi.display_res_count = uno_pc64_res_count;
    gKApi.display_res_get   = uno_pc64_res_get;
    gKApi.display_res_set   = uno_pc64_res_set;
}

static const AppInterface *iface(short proc)
{
    if (proc < 0 || proc >= APP_NAPPS) return 0;
    if (!gTried[proc]) {
        UnoAppEntry e = entry_for(proc);
        gTried[proc] = 1;
        if (e) gIface[proc] = e(&gKApi);
    }
    return gIface[proc];
}

/* ---- the shell-facing surface (index-based, hides uno_app.h's APP_* enum) --
 * The shell refers to migrated apps by a dense 0..UNOAPP_COUNT-1 index; the
 * bridge maps each to its proc id. Games prefer fullscreen. */
#include "pc64_uui_apps.h"

static const short kProc[UNOAPP_COUNT] =
    { APP_DOSTRIS, APP_PACMAN, APP_OUTLAST, APP_MUSIC, APP_TRACKER, APP_PAINT, APP_NETWORK };
static const signed char kGame[UNOAPP_COUNT] = { 1, 1, 1, 0, 0, 0, 0 };
static const char *kName[UNOAPP_COUNT] =
    { "Dostris", "Pac-Man", "OutLast", "Music", "Tracker", "Paint", "Network" };

const char *unoapp_name(int i) { return (i >= 0 && i < UNOAPP_COUNT) ? kName[i] : "App"; }
int unoapp_is_game(int i)      { return (i >= 0 && i < UNOAPP_COUNT) ? kGame[i] : 0; }

void unoapp_size(int i, int *w, int *h)
{
    const AppInterface *a = (i >= 0 && i < UNOAPP_COUNT) ? iface(kProc[i]) : 0;
    if (a) { *w = a->win_rect[2] - a->win_rect[0]; *h = a->win_rect[3] - a->win_rect[1]; }
    else   { *w = 280; *h = 200; }
}

void unoapp_open(int i)
{
    short proc; const AppInterface *a;
    if (i < 0 || i >= UNOAPP_COUNT) return;
    proc = kProc[i]; a = iface(proc);
    gAppLive[proc] = true;
    if (a && a->opened) a->opened();
}
void unoapp_close(int i)
{
    short proc; const AppInterface *a;
    if (i < 0 || i >= UNOAPP_COUNT) return;
    proc = kProc[i]; a = iface(proc);
    if (a && a->closed) a->closed();
    gAppLive[proc] = false;
}

/* paint app i into its canvas rect: the app draws relative to bounds.left /
 * bounds.top+TBAR_H, so offset the bounds so its content lands in the rect. */
void unoapp_paint(int i, unoui_rect r)
{
    short proc; const AppInterface *a; UnoWin *w;
    if (i < 0 || i >= UNOAPP_COUNT) return;
    proc = kProc[i]; a = iface(proc);
    if (!a || !a->draw) return;
    w = &gAppWins[proc];
    w->used = true; w->proc = proc; w->title = a->win_title;
    w->bounds.left = (short)r.x;          w->bounds.top    = (short)(r.y - TBAR_H);
    w->bounds.right = (short)(r.x + r.w); w->bounds.bottom = (short)(r.y + r.h);
    a->draw(w);
}

/* map a unoui event to app i's key/click; returns 1 if the app consumed it */
int unoapp_input(int i, const unoui_event *ev)
{
    short proc; const AppInterface *a;
    if (i < 0 || i >= UNOAPP_COUNT) return 0;
    proc = kProc[i]; a = iface(proc);
    if (!a) return 0;
    if (ev->kind == UI_EV_CHAR && a->key)
        return a->key((char)ev->ch, 0, (ev->mods & UI_MOD_CTRL) != 0) ? 1 : 0;
    if (ev->kind == UI_EV_KEY && a->key) {
        char ch = 0; short code = 0;
        switch (ev->key) {                 /* arrows -> Mac ctrl chars + keycodes */
        case UI_KEY_LEFT:  ch = 0x1C; code = 0x7B; break;
        case UI_KEY_RIGHT: ch = 0x1D; code = 0x7C; break;
        case UI_KEY_DOWN:  ch = 0x1F; code = 0x7D; break;
        case UI_KEY_UP:    ch = 0x1E; code = 0x7E; break;
        case UI_KEY_ENTER: ch = 0x0D; break;
        case UI_KEY_BACKSPACE: ch = 0x08; break;
        default: return 0;
        }
        return a->key(ch, code, (ev->mods & UI_MOD_CTRL) != 0) ? 1 : 0;
    }
    if (ev->kind == UI_EV_MOUSE_DOWN && a->click) {
        Point p; p.h = (short)ev->x; p.v = (short)ev->y;
        a->click(&gAppWins[proc], p); return 1;
    }
    return 0;
}

/* per-frame: advance app i's clock + the game-music sequencer */
void unoapp_focus(int i) { gActiveProc = (i >= 0 && i < UNOAPP_COUNT) ? kProc[i] : -1; }
void unoapp_run_tick(int i)
{
    const AppInterface *a = (i >= 0 && i < UNOAPP_COUNT) ? iface(kProc[i]) : 0;
    if (a && a->tick) a->tick();
    gm_tick();
}
void unoapp_setup(int *dirtyflag) { unoapp_init(dirtyflag); }

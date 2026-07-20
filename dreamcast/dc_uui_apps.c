/* ===========================================================================
 * UnoDOS/Dreamcast - the LEGACY-APP BRIDGE for the unoui shell.
 *
 * The 11 roster apps (apps/*.c) are self-contained AppInterface modules that
 * draw through a KernelApi of Toolbox-backed primitives. Rather than rewrite
 * them against unoui widgets, this bridge hosts them UNCHANGED as unoui
 * canvases: unoui owns the window/desktop/taskbar/z-order, and each app just
 * paints into its canvas rect and receives mapped input. The DC analogue of
 * pc64/pc64_uui_apps.c.
 *
 * It supplies its own compact KernelApi (drawing + util copied from unodos.c's
 * colour path; sound rides the Sound Manager shim in mac_io.c, which drives
 * the AICA; files ride FSOpen/... over the VMU). None of unodos.c's window
 * manager is linked - the Toolbox primitives come from mac_compat.c.
 *
 * App linkage: the uui build statically links the 11 app cores, each compiled
 * with -DUNO_APP_SYM=uno_app_main_<name> (the same rename the classic-Mac
 * native build uses), so the .KLF load-from-CD pipeline is not needed here.
 * Decoupling purity stays with the legacy build; the shell wants a roster.
 * ======================================================================== */
#include "uno_app.h"        /* AppInterface, KernelApi, mac_compat.h (Toolbox) */
#include "unoui.h"
#include "uui_apps.h"
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

/* ---- sound: the Sound Manager shim (mac_io.c) -> platform synth -----------
 * Same channel model as unodos.c: one square-wave channel shared by the Music
 * app, the Tracker and the game-music sequencer below. mac_io.c realises
 * noteCmd/quietCmd on the platform synth (DC: AICA via uno_dc_snd_*). */
static SndChannelPtr gSnd = NULL;

static void music_open_chan(void)
{
    if (gSnd) return;
    if (SndNewChannel(&gSnd, noteSynth, 0, NULL) != noErr)
        gSnd = NULL;                /* no sound HW: visual only */
}
static void music_note_on(short midi, short durTicks)
{
    SndCommand c;
    if (!gSnd) return;
    c.cmd = noteCmd;
    c.param1 = (short)(durTicks * 33);      /* ticks(1/60s) -> half-ms */
    c.param2 = midi;
    SndDoImmediate(gSnd, &c);
}
static void music_quiet(void)
{
    SndCommand c;
    if (!gSnd) return;
    c.cmd = quietCmd; c.param1 = 0; c.param2 = 0;
    SndDoImmediate(gSnd, &c);
}
static void music_start(void) { music_open_chan(); }
static void music_stop(void)  { music_quiet(); }

/* game music: Korobeiniki / Sunset Drive loops. Owner-gated: only the focused
 * app's song plays (the legacy core gated on the topmost window). */
static short gActiveProc = -1;             /* focused app (kapi_topmost too) */
static const Note *gGmNotes = NULL;
static short gGmCount = 0, gGmIx = 0, gGmOwner = -1;
static long  gGmEnd = 0;
static Boolean gGmOn = false;

static void gm_start(const Note *notes, short count, short owner)
{
    if (!gSnd) music_open_chan();
    gGmNotes = notes; gGmCount = count; gGmOwner = owner;
    gGmIx = count - 1;                  /* first tick wraps to note 0 */
    gGmOn = true;
    gGmEnd = TickCount();
}
static void gm_stop(void)
{
    gGmOn = false;
    music_quiet();
}
static void gm_tick(void)
{
    if (!gGmOn || !gGmNotes) return;
    if (gActiveProc != gGmOwner) return;
    if (TickCount() < gGmEnd) return;
    gGmIx++;
    if (gGmIx >= gGmCount) gGmIx = 0;
    gGmEnd = TickCount() + gGmNotes[gGmIx].dur;
    if (gGmNotes[gGmIx].midi) music_note_on(gGmNotes[gGmIx].midi, gGmNotes[gGmIx].dur);
    else music_quiet();
}

/* ---- window-manager hooks -> "mark the shell dirty" ---------------------- */
static int    *gDirty;
static UnoWin  gAppWins[APP_NAPPS];
static Boolean gAppLive[APP_NAPPS];
static UnoWin *find_app_window(short proc)
{ return (proc >= 0 && proc < APP_NAPPS && gAppLive[proc]) ? &gAppWins[proc] : 0; }
static void draw_window(UnoWin *w) { (void)w; if (gDirty) *gDirty = 1; }
static void repaint_all(void)      { if (gDirty) *gDirty = 1; }
static void launch_app(short proc) { uui_shell_launch(proc); }
static short kapi_topmost(void)    { return gActiveProc; }

/* ---- files: Paint/Tracker save/load, wired to the platform File Manager
 * (mac_io.c - the VMU on DC), same path Files/Notepad already use. ---------- */
static short         gFatCount;
static unsigned char gFatNames[16][13];
static long          gFatSizes[16];
static void bc2p(const char *s, unsigned char *p)   /* C string -> Pascal string */
{ int n = 0; while (s[n] && n < 31) n++; p[0] = (unsigned char)n;
  { int i; for (i = 0; i < n; i++) p[i + 1] = (unsigned char)s[i]; } }
static Boolean fat12_mount(void) { return true; }
static void    fat12_list(void)                  /* enumerate the volume root */
{
    CInfoPBRec pb;
    unsigned char pname[34];
    int idx = 1;
    gFatCount = 0;
    for (idx = 1; gFatCount < 16; idx++) {
        memset(&pb, 0, sizeof pb);
        pb.dirInfo.ioNamePtr = pname;
        pb.dirInfo.ioFDirIndex = (short)idx;
        if (PBGetCatInfoSync(&pb) != noErr) break;
        if (pb.dirInfo.ioFlAttrib & 0x10) continue;      /* skip directories */
        { int n = pname[0], j; if (n > 12) n = 12;
          for (j = 0; j < n; j++) gFatNames[gFatCount][j] = pname[j + 1];
          gFatNames[gFatCount][n] = 0; }
        gFatSizes[gFatCount] = pb.dirInfo.ioFlLgLen;
        gFatCount++;
    }
}
static long    fat12_read(const char *name, unsigned char *buf, long max)
{
    unsigned char pn[34]; short ref; long cnt = max;
    bc2p(name, pn);
    if (FSOpen(pn, 0, &ref) != noErr) return 0;
    if (FSRead(ref, &cnt, buf) != noErr && cnt <= 0) cnt = 0;
    FSClose(ref);
    return cnt > 0 ? cnt : 0;
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

/* ---- the statically linked app registry -----------------------------------
 * Each apps/<name>.c is compiled with -DUNO_APP_SYM=uno_app_main_<name>, so
 * all 11 entries co-link into one image (the classic-Mac native pattern). */
#define DECL(n) extern const AppInterface *uno_app_main_##n(const KernelApi *)
DECL(sysinfo); DECL(clock);  DECL(files);   DECL(notepad); DECL(music);
DECL(dostris); DECL(outlast); DECL(pacman); DECL(tracker); DECL(paint);
DECL(theme);
#undef DECL

static UnoAppEntry const kEntry[APP_NAPPS] = {   /* uno_app.h enum order */
    uno_app_main_sysinfo, uno_app_main_clock,   uno_app_main_files,
    uno_app_main_notepad, uno_app_main_music,   uno_app_main_dostris,
    uno_app_main_outlast, uno_app_main_pacman,  uno_app_main_tracker,
    uno_app_main_paint,   uno_app_main_theme
};

/* ---- the KernelApi handed to every app ----------------------------------- */
static KernelApi gKApi;
static const AppInterface *gIface[APP_NAPPS];
static int                 gTried[APP_NAPPS];

void unoapp_setup(int *dirtyflag)
{
    InitGraf(&gKApi);               /* seed qd (screen bounds) for the apps */
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
}

static const AppInterface *iface(short proc)
{
    if (proc < 0 || proc >= APP_NAPPS) return 0;
    if (!gTried[proc]) {
        gTried[proc] = 1;
        if (kEntry[proc]) gIface[proc] = kEntry[proc](&gKApi);
    }
    return gIface[proc];
}

/* ---- the shell-facing surface (index == proc, dense already) ------------- */
static const char *kName[UNOAPP_COUNT] =
    { "SysInfo", "Clock", "Files", "Notepad", "Music",
      "Dostris", "OutLast", "Pac-Man", "Tracker", "Paint", "Theme" };
static const signed char kGame[UNOAPP_COUNT] =
    { 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0 };

const char *unoapp_name(int i) { return (i >= 0 && i < UNOAPP_COUNT) ? kName[i] : "App"; }
int unoapp_is_game(int i)      { return (i >= 0 && i < UNOAPP_COUNT) ? kGame[i] : 0; }

void unoapp_size(int i, int *w, int *h)
{
    const AppInterface *a = (i >= 0 && i < UNOAPP_COUNT) ? iface((short)i) : 0;
    if (a) { *w = a->win_rect[2] - a->win_rect[0]; *h = a->win_rect[3] - a->win_rect[1]; }
    else   { *w = 280; *h = 200; }
}

void unoapp_open(int i)
{
    const AppInterface *a;
    if (i < 0 || i >= UNOAPP_COUNT) return;
    a = iface((short)i);
    gAppLive[i] = true;
    if (a && a->opened) a->opened();
}
void unoapp_close(int i)
{
    const AppInterface *a;
    if (i < 0 || i >= UNOAPP_COUNT) return;
    a = iface((short)i);
    if (a && a->closed) a->closed();
    gAppLive[i] = false;
}

/* paint app i into its canvas rect: the app draws relative to bounds.left /
 * bounds.top+TBAR_H, so offset the bounds so its content lands in the rect. */
void unoapp_paint(int i, unoui_rect r)
{
    const AppInterface *a; UnoWin *w;
    if (i < 0 || i >= UNOAPP_COUNT) return;
    a = iface((short)i);
    if (!a || !a->draw) {                  /* entry missing / returned NULL */
        text_at((short)(r.x + 8), (short)(r.y + 24), "app core not linked",
                C_BLUE, C_WHITE, false);
        return;
    }
    w = &gAppWins[i];
    w->used = true; w->proc = (short)i; w->title = a->win_title;
    w->bounds.left = (short)r.x;          w->bounds.top    = (short)(r.y - TBAR_H);
    w->bounds.right = (short)(r.x + r.w); w->bounds.bottom = (short)(r.y + r.h);
    a->draw(w);
}

/* map a unoui event to app i's key/click; returns 1 if the app consumed it */
int unoapp_input(int i, const unoui_event *ev)
{
    const AppInterface *a;
    if (i < 0 || i >= UNOAPP_COUNT) return 0;
    a = iface((short)i);
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
        case UI_KEY_TAB:   ch = 0x09; break;
        default: return 0;
        }
        return a->key(ch, code, (ev->mods & UI_MOD_CTRL) != 0) ? 1 : 0;
    }
    if (ev->kind == UI_EV_MOUSE_DOWN && a->click) {
        Point p; p.h = (short)ev->x; p.v = (short)ev->y;
        a->click(&gAppWins[i], p); return 1;
    }
    return 0;
}

/* per-frame: advance app i's clock + the game-music sequencer */
void unoapp_focus(int i) { gActiveProc = (i >= 0 && i < UNOAPP_COUNT) ? (short)i : -1; }
void unoapp_run_tick(int i)
{
    const AppInterface *a = (i >= 0 && i < UNOAPP_COUNT) ? iface((short)i) : 0;
    if (a && a->tick) a->tick();
    gm_tick();
}

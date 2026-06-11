/* ===========================================================================
 * UnoDOS/Mac - classic Mac OS port (milestone 1)
 * ===========================================================================
 * One codebase, two applications (selected by the UNO_COLOR compile flag):
 *
 *   UnoDOS7       (-DUNO_COLOR=1)  System 7 era, Color QuickDraw, Mac II /
 *                                  LC / Quadra family. Full UnoDOS palette.
 *   UnoDOSClassic (mono)           System 1-6, classic 1-bit QuickDraw,
 *                                  Mac Plus / SE / Classic (68000). No Color
 *                                  QuickDraw calls at all (it did not exist
 *                                  before the Mac II), so this build runs on
 *                                  the earliest hardware.
 *
 * Per docs/PORT-SPEC.md and the Toolbox-based strategy in
 * docs/M68K-PORT-FEASIBILITY.md: UnoDOS owns ONE full-screen GrafPort and
 * runs its OWN window manager / widgets / theme inside it. The ROM Toolbox
 * supplies the screen, the Event Manager (mouse + keyboard, already
 * press-time stamped), QuickDraw primitives, and TickCount. The window
 * manager, event routing, desktop, and apps are the same model as the x86
 * and Amiga ports.
 *
 * Audit-derived rules carried over (PORT-SPEC SS6): hit-test the PRESS
 * location the Event Manager hands us (never the drifted current position),
 * edge-only mouse handling, topmost-only periodic content refresh.
 * ===========================================================================
 */

#include <Quickdraw.h>
#include <Windows.h>
#include <Fonts.h>
#include <Events.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <ToolUtils.h>
#include <OSUtils.h>
#include <Timer.h>
#include <string.h>

/* ---- QuickDraw globals (Retro68 provides `qd`) ------------------------- */
QDGlobals qd;

/* ---- UnoDOS palette (PORT-SPEC SS1) ------------------------------------ */
enum { C_BLUE = 0, C_CYAN = 1, C_MAG = 2, C_WHITE = 3 };

#if UNO_COLOR
static RGBColor kPalette[4] = {
    { 0x0000, 0x0000, 0xAAAA },   /* desktop blue   */
    { 0x0000, 0xAAAA, 0xAAAA },   /* cyan accent    */
    { 0xAAAA, 0x0000, 0xAAAA },   /* magenta accent */
    { 0xFFFF, 0xFFFF, 0xFFFF }    /* white          */
};
static RGBColor kBlack = { 0, 0, 0 };
#endif

/* ---- layout (mirrors PORT-SPEC desktop metrics) ------------------------ */
#define TBAR_H     18
#define MENUBAR_H  20
#define ICON_PITCH 96
#define ICON0_X    40
#define ICON0_Y    44
#define NICONS     2
#define MAXWIN     6
#define DBLCLICK   30            /* ticks (~0.5s @ 60Hz) */

typedef struct {
    Boolean used;
    short   proc;               /* app index */
    Rect    bounds;             /* whole window incl. title bar */
    const char *title;
} UnoWin;

/* ---- state ------------------------------------------------------------- */
static WindowPtr gWin;          /* our single full-screen GrafPort */
static Rect   gScreen;          /* full screen bounds */
static UnoWin gWins[MAXWIN];
static short  gZ[MAXWIN];       /* z order: gZ[count-1] = topmost */
static short  gZCount = 0;
static short  gSel = 0;         /* selected desktop icon */
static long   gBootTicks;
static long   gLastSec = -1;
static short  gDblIcon = -1;
static long   gDblTick = 0;

/* drag state */
static Boolean gDragging = false;
static short  gDragWin = -1;
static short  gDragDX, gDragDY;
static Rect   gDragOutline;
static Boolean gOutlineShown = false;

static const char *kIconNames[NICONS] = { "Sys Info", "Clock" };
static const char *kWinTitles[NICONS] = { "System Info", "Clock" };

/* =========================================================================
 * Theme layer - the ONLY part that differs between the color and mono apps
 * ========================================================================= */
static void theme_fore(short c)
{
#if UNO_COLOR
    RGBForeColor(&kPalette[c]);
#else
    /* 1-bit: frames/text/accents are black; "white" is white */
    ForeColor(c == C_WHITE ? whiteColor : blackColor);
#endif
}
static void theme_back(short c)
{
#if UNO_COLOR
    RGBBackColor(&kPalette[c]);
#else
    BackColor(c == C_WHITE ? whiteColor : blackColor);
#endif
}

/* Theme model:
 *  COLOR (System 7): the UnoDOS 4-colour palette literally.
 *  MONO  (System 1-6): authentic 1-bit Mac. The blue desktop becomes the
 *    classic 50%% gray pattern; window CONTENT is white; everything drawn
 *    (frames, title bars, glyphs, text) is black-on-white. Accent colours
 *    (cyan/magenta) collapse to black. This is the standard look of the
 *    era and stays perfectly legible on a 1-bit screen. */

/* desktop background fill (the only place the "blue" reads as a backdrop) */
static void desktop_bg(Rect *r)
{
#if UNO_COLOR
    RGBForeColor(&kPalette[C_BLUE]);
    PaintRect(r);
    RGBForeColor(&kBlack);
#else
    FillRect(r, &qd.gray);
#endif
}

/* paint a solid rect in palette colour c (C_BLUE here = window content) */
static void uno_fill(Rect *r, short c)
{
#if UNO_COLOR
    RGBForeColor(&kPalette[c]);
    PaintRect(r);
    RGBForeColor(&kBlack);
#else
    if (c == C_WHITE || c == C_BLUE) FillRect(r, &qd.white);  /* content/title = white */
    else                             FillRect(r, &qd.black);  /* accents = black */
#endif
}

static void theme_black(void)
{
#if UNO_COLOR
    RGBForeColor(&kBlack);
#else
    ForeColor(blackColor);
#endif
}

/* 1px frame in palette colour c (frames are always black in mono) */
static void uno_box(Rect *r, short c)
{
#if UNO_COLOR
    RGBForeColor(&kPalette[c]);
    FrameRect(r);
    RGBForeColor(&kBlack);
#else
    ForeColor(blackColor);
    FrameRect(r);
#endif
}

/* draw a C string at (x,y) baseline. In mono everything is black-on-white;
 * `opaque` paints a white background box (used for desktop labels over the
 * gray backdrop so they stay readable). */
static void text_at(short x, short y, const char *s, short fg, short bg, Boolean opaque)
{
    short len = (short)strlen(s);
    MoveTo(x, y);
#if UNO_COLOR
    RGBForeColor(&kPalette[fg]);
    if (opaque) { RGBBackColor(&kPalette[bg]); TextMode(srcCopy); }
    else        { TextMode(srcOr); }
    DrawText((Ptr)s, 0, len);
    RGBForeColor(&kBlack);
    RGBBackColor(&kPalette[C_WHITE]);
    TextMode(srcOr);
#else
    (void)fg; (void)bg;
    ForeColor(blackColor);
    BackColor(whiteColor);
    TextMode(opaque ? srcCopy : srcOr);   /* opaque = white halo on the gray desktop */
    DrawText((Ptr)s, 0, len);
    TextMode(srcOr);
#endif
}

/* =========================================================================
 * Small helpers
 * ========================================================================= */
static long now_secs(void) { return (TickCount() - gBootTicks) / 60; }

static void fmt_u(long v, char *out)            /* unsigned decimal */
{
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
    int i = 0; while (n) out[i++] = tmp[--n];
    out[i] = 0;
}
static void put2(long v, char *out) { out[0]='0'+(v/10)%10; out[1]='0'+v%10; out[2]=0; }

/* =========================================================================
 * Window manager (same model as the x86/Amiga ports; PORT-SPEC SS2)
 * ========================================================================= */
static UnoWin *zwin(short z)        { return &gWins[gZ[z]]; }

static short find_window_at(Point p)   /* topmost hit, -1 = none */
{
    short z;
    for (z = gZCount - 1; z >= 0; z--) {
        Rect *b = &zwin(z)->bounds;
        if (p.h >= b->left && p.h < b->right &&
            p.v >= b->top  && p.v < b->bottom)
            return z;
    }
    return -1;
}

static void draw_app_content(short proc, UnoWin *w);  /* fwd */

static void draw_window(UnoWin *w)
{
    Rect r = w->bounds, tb, ct;
    /* content background */
    ct = r; ct.top += TBAR_H; InsetRect(&ct, 1, 1); uno_fill(&ct, C_BLUE);
    /* title bar */
    tb = r; tb.bottom = tb.top + TBAR_H; uno_fill(&tb, C_WHITE);
    /* border */
    uno_box(&r, C_WHITE);
    { Rect sep = tb; sep.top = sep.bottom - 1; theme_fore(C_BLUE);
      MoveTo(sep.left, sep.top); LineTo(sep.right - 1, sep.top); }
    /* title text (blue on white) */
    text_at(r.left + 6, r.top + 13, w->title, C_BLUE, C_WHITE, true);
    /* close uno_box glyph */
    text_at(r.right - 14, r.top + 13, "X", C_BLUE, C_WHITE, true);
    /* content */
    draw_app_content(w->proc, w);
}

static void draw_desktop(void);     /* fwd */

static void repaint_all(void)
{
    short z;
    draw_desktop();
    for (z = 0; z < gZCount; z++)
        draw_window(zwin(z));
}

static void raise_window(short z)
{
    short i, slot;
    if (z == gZCount - 1) return;       /* already topmost */
    slot = gZ[z];
    for (i = z; i < gZCount - 1; i++) gZ[i] = gZ[i + 1];
    gZ[gZCount - 1] = slot;
    repaint_all();
}

static void close_window(short z)
{
    short i;
    gWins[gZ[z]].used = false;
    for (i = z; i < gZCount - 1; i++) gZ[i] = gZ[i + 1];
    gZCount--;
    repaint_all();
}

static void launch_app(short proc)
{
    short i, slot = -1;
    /* already open? raise it */
    for (i = 0; i < gZCount; i++)
        if (gWins[gZ[i]].proc == proc) { raise_window(i); return; }
    for (i = 0; i < MAXWIN; i++) if (!gWins[i].used) { slot = i; break; }
    if (slot < 0) return;               /* table full (defined behavior) */

    gWins[slot].used  = true;
    gWins[slot].proc  = proc;
    gWins[slot].title = kWinTitles[proc];
    if (proc == 0) SetRect(&gWins[slot].bounds, 40, 50, 320, 170);   /* SysInfo */
    else           SetRect(&gWins[slot].bounds, 120, 80, 320, 180);  /* Clock   */
    gZ[gZCount++] = slot;
    draw_window(&gWins[slot]);          /* topmost: nothing else to repaint */
}

/* =========================================================================
 * Apps (in-app window procs)
 * ========================================================================= */
static void draw_app_content(short proc, UnoWin *w)
{
    short x = w->bounds.left + 8;
    short y = w->bounds.top + TBAR_H + 14;
    char num[16];

    if (proc == 0) {                    /* SysInfo */
        char line[40];
        text_at(x, y,       "Video", C_WHITE, C_BLUE, false);
#if UNO_COLOR
        text_at(x + 80, y,  "Color QuickDraw", C_CYAN, C_BLUE, false);
        text_at(x, y + 16,  "System", C_WHITE, C_BLUE, false);
        text_at(x + 80, y + 16, "7.x (Mac II+)", C_CYAN, C_BLUE, false);
#else
        text_at(x + 80, y,  "1-bit QuickDraw", C_WHITE, C_BLUE, false);
        text_at(x, y + 16,  "System", C_WHITE, C_BLUE, false);
        text_at(x + 80, y + 16, "1-6 (68000)", C_WHITE, C_BLUE, false);
#endif
        text_at(x, y + 32,  "Uptime", C_WHITE, C_BLUE, false);
        fmt_u(now_secs(), num); strcpy(line, num); strcat(line, "s   ");
        text_at(x + 80, y + 32, line, C_CYAN, C_BLUE, true);
        text_at(x, y + 54, "UnoDOS/Mac  Milestone 1", C_MAG, C_BLUE, false);
    } else {                            /* Clock - uptime as HH:MM:SS */
        long s = now_secs();
        char buf[12];
        put2(s / 3600, buf);     buf[2] = ':';
        put2((s / 60) % 60, buf + 3); buf[5] = ':';
        put2(s % 60, buf + 6);   buf[8] = 0;
        text_at(x, y, "Uptime", C_WHITE, C_BLUE, false);
        {
            short cx = w->bounds.left + (w->bounds.right - w->bounds.left) / 2 - 28;
            short cy = w->bounds.top + (w->bounds.bottom - w->bounds.top) / 2 + 8;
            text_at(cx, cy, buf, C_CYAN, C_BLUE, true);
        }
    }
}

/* =========================================================================
 * Desktop
 * ========================================================================= */
static void icon_rect(short i, Rect *r)
{
    short x = ICON0_X + i * ICON_PITCH;
    SetRect(r, x - 4, ICON0_Y - 4, x + 24, ICON0_Y + 24);
}

static void draw_icon(short i)
{
    Rect cell, g;
    short x = ICON0_X + i * ICON_PITCH;
    icon_rect(i, &cell);
    desktop_bg(&cell);                                /* clear cell to backdrop */
    /* simple themed glyph */
    if (i == 0) {                                 /* SysInfo: monitor */
        SetRect(&g, x, ICON0_Y, x + 18, ICON0_Y + 13);
        uno_box(&g, C_CYAN);
        { Rect inr = g; InsetRect(&inr, 2, 2); uno_fill(&inr, C_CYAN); }
        SetRect(&g, x + 6, ICON0_Y + 13, x + 12, ICON0_Y + 16); uno_box(&g, C_CYAN);
    } else {                                      /* Clock: circle */
        SetRect(&g, x, ICON0_Y, x + 16, ICON0_Y + 16);
        theme_fore(C_CYAN); FrameOval(&g); theme_fore(C_BLUE);
        MoveTo(x + 8, ICON0_Y + 8); LineTo(x + 8, ICON0_Y + 3);
        MoveTo(x + 8, ICON0_Y + 8); LineTo(x + 12, ICON0_Y + 8);
    }
    /* label */
    text_at(x - 8, ICON0_Y + 28, kIconNames[i], C_WHITE, C_BLUE, true);
    /* selection uno_box */
    if (i == gSel) {
#if UNO_COLOR
        uno_box(&cell, C_WHITE);
#else
        PenMode(patXor); FrameRect(&cell); PenNormal();
#endif
    }
}

static void draw_desktop(void)
{
    short i;
    desktop_bg(&gScreen);
    text_at(gScreen.left + 6, gScreen.top + 14, "UnoDOS Mac", C_WHITE, C_BLUE, true);
#if UNO_COLOR
    text_at(gScreen.left + 6, gScreen.bottom - 8, "UnoDOS/Mac 7  v0.1.0", C_WHITE, C_BLUE, true);
#else
    text_at(gScreen.left + 6, gScreen.bottom - 8, "UnoDOS/Mac Classic  v0.1.0", C_WHITE, C_BLUE, true);
#endif
    for (i = 0; i < NICONS; i++) draw_icon(i);
}

static short icon_at(Point p)
{
    short i; Rect r;
    for (i = 0; i < NICONS; i++) {
        icon_rect(i, &r);
        if (PtInRect(p, &r)) return i;
    }
    return -1;
}

static void select_icon(short i)
{
    short old = gSel;
    if (i == old) return;
    gSel = i;
    if (old >= 0) draw_icon(old);
    draw_icon(i);
}

/* =========================================================================
 * Drag (self-erasing XOR outline; clamp per PORT-SPEC SS2)
 * ========================================================================= */
static void xor_outline(Rect *r)
{
    PenMode(patXor);
#if UNO_COLOR
    PenPat(&qd.gray);
#else
    PenPat(&qd.black);
#endif
    FrameRect(r);
    PenNormal();
}

static void drag_update(Point mouse, Boolean stillDown)
{
    UnoWin *w = &gWins[gDragWin];
    short ww = w->bounds.right - w->bounds.left;
    short wh = w->bounds.bottom - w->bounds.top;
    short nx = mouse.h - gDragDX;
    short ny = mouse.v - gDragDY;
    Rect target;

    if (nx < 0) nx = 0;
    if (nx > gScreen.right - ww) nx = gScreen.right - ww;
    if (ny < MENUBAR_H) ny = MENUBAR_H;
    if (ny > gScreen.bottom - TBAR_H) ny = gScreen.bottom - TBAR_H;
    SetRect(&target, nx, ny, nx + ww, ny + wh);

    if (stillDown) {
        if (gOutlineShown &&
            target.left == gDragOutline.left && target.top == gDragOutline.top)
            return;
        if (gOutlineShown) xor_outline(&gDragOutline);
        gDragOutline = target; gOutlineShown = true;
        xor_outline(&gDragOutline);
    } else {
        if (gOutlineShown) { xor_outline(&gDragOutline); gOutlineShown = false; }
        w->bounds = target;
        gDragging = false;
        repaint_all();
    }
}

/* =========================================================================
 * Mouse / keyboard handling
 * ========================================================================= */
static void on_mouse_down(Point p)      /* p = press location, local coords */
{
    short z = find_window_at(p);
    if (z >= 0) {
        UnoWin *w = zwin(z);
        if (p.v < w->bounds.top + TBAR_H) {     /* title bar */
            if (p.h >= w->bounds.right - 14) { close_window(z); return; }
            raise_window(z);
            /* re-find: it is now topmost */
            gDragWin = gZ[gZCount - 1];
            gDragDX = p.h - gWins[gDragWin].bounds.left;
            gDragDY = p.v - gWins[gDragWin].bounds.top;
            gDragging = true; gOutlineShown = false;
        } else {                                /* body: raise only */
            if (z != gZCount - 1) raise_window(z);
        }
        return;
    }
    /* desktop: icon hit-test at the press location (PORT-SPEC rule 4) */
    {
        short i = icon_at(p);
        long t = TickCount();
        if (i < 0) { gDblIcon = -1; return; }
        if (i == gDblIcon && (t - gDblTick) <= DBLCLICK) {
            gDblIcon = -1; select_icon(i); launch_app(i);
        } else {
            gDblIcon = i; gDblTick = t; select_icon(i);
        }
    }
}

static void on_key(char ch, short code)
{
    if (code == 0x7C || ch == 0x1D) {           /* right arrow */
        select_icon((gSel + 1) % NICONS);
    } else if (code == 0x7B || ch == 0x1C) {    /* left arrow */
        select_icon((gSel + NICONS - 1) % NICONS);
    } else if (ch == 0x0D || ch == 0x03) {      /* Return / Enter */
        launch_app(gSel);
    } else if (ch == 0x1B) {                    /* ESC: close topmost */
        if (gZCount > 0) close_window(gZCount - 1);
    }
}

/* once a second, refresh the topmost window's content (single-topmost) */
static void app_tick(void)
{
    long s = now_secs();
    if (s == gLastSec) return;
    gLastSec = s;
    if (gZCount > 0) {
        UnoWin *w = zwin(gZCount - 1);
        draw_app_content(w->proc, w);
    }
}

/* =========================================================================
 * Boot
 * ========================================================================= */
static void init_toolbox(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
}

int main(void)
{
    Rect r;
    EventRecord e;

    init_toolbox();
    gScreen = qd.screenBits.bounds;

    /* one full-screen borderless GrafPort; we draw everything inside it */
    r = gScreen;
#if UNO_COLOR
    gWin = NewCWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1L, false, 0);
#else
    gWin = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1L, false, 0);
#endif
    SetPort(gWin);
    TextFont(0);            /* system font (Chicago) - authentic Mac look */
    TextSize(12);

    gBootTicks = TickCount();
    gSel = 0;
    draw_desktop();

#ifdef UNO_AUTOTEST
    /* Auto-launch both apps so the WM + app procs can be verified without
       host->guest input injection (the Amiga port uses the same trick).
       Build with -DUNO_AUTOTEST. */
    launch_app(1);      /* Clock */
    launch_app(0);      /* SysInfo (ends up topmost) */
#endif

    for (;;) {
        /* WaitNextEvent if available, else GetNextEvent + a short poll */
        Boolean got;
        got = GetNextEvent(everyEvent, &e);
        if (got) {
            switch (e.what) {
            case mouseDown: {
                Point p = e.where; GlobalToLocal(&p);
                on_mouse_down(p);
                break;
            }
            case keyDown:
            case autoKey: {
                char ch = e.message & charCodeMask;
                short code = (e.message & keyCodeMask) >> 8;
                /* Cmd-Q quits (lets the emulator return cleanly) */
                if ((e.modifiers & cmdKey) && (ch == 'q' || ch == 'Q')) return 0;
                on_key(ch, code);
                break;
            }
            }
        }
        if (gDragging) {
            Point p; Boolean down;
            GetMouse(&p);
            down = StillDown();
            drag_update(p, down);
        }
        app_tick();
    }
    return 0;
}

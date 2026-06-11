/* ===========================================================================
 * UnoDOS/Mac - classic Mac OS port (milestone 2)
 * ===========================================================================
 * One codebase, two applications (selected by the UNO_COLOR compile flag):
 *
 *   UnoDOS7       (-DUNO_COLOR=1)  System 7 era, Color QuickDraw, Mac II /
 *                                  LC / Quadra family. Full UnoDOS palette.
 *   UnoDOSClassic (mono)           System 1-6, classic 1-bit QuickDraw,
 *                                  Mac Plus / SE / Classic (68000). No Color
 *                                  QuickDraw calls at all.
 *
 * Per docs/PORT-SPEC.md and the Toolbox-based strategy in
 * docs/M68K-PORT-FEASIBILITY.md: UnoDOS owns ONE full-screen GrafPort and
 * runs its OWN window manager / widgets / theme inside it. The ROM Toolbox
 * supplies the screen, the Event Manager (mouse + keyboard, already
 * press-time stamped), QuickDraw primitives, TickCount, the File Manager
 * (Files/Notepad storage) and the Sound Manager (Music).
 *
 * Milestone 2 adds the x86 app set's core trio:
 *   Files   - volume directory listing via PBGetCatInfo, open file in
 *             Notepad (Enter / double-click)
 *   Notepad - text editor: caret, insert/backspace/return/arrows, live
 *             Ln/Col/byte status bar (the x86 audit's stale-status fix is
 *             LAW here: status redraws on every edit), Cmd-S save via the
 *             File Manager
 *   Music   - Canon in D (same arrangement as apps/music.asm) on the
 *             Sound Manager square-wave synth, with a staff view and a
 *             moving playback highlight
 *
 * Audit-derived rules carried over (PORT-SPEC SS6): hit-test the PRESS
 * location, edge-only mouse handling, topmost-only periodic refresh,
 * focused (topmost) window owns the keyboard.
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
#include <Files.h>
#include <Sound.h>
#include <string.h>

QDGlobals qd;

/* ---- UnoDOS palette (PORT-SPEC SS1) ------------------------------------ */
enum { C_BLUE = 0, C_CYAN = 1, C_MAG = 2, C_WHITE = 3 };

#if UNO_COLOR
static RGBColor kPalette[4] = {
    { 0x0000, 0x0000, 0xAAAA },
    { 0x0000, 0xAAAA, 0xAAAA },
    { 0xAAAA, 0x0000, 0xAAAA },
    { 0xFFFF, 0xFFFF, 0xFFFF }
};
static RGBColor kBlack = { 0, 0, 0 };
#endif

/* ---- layout ------------------------------------------------------------- */
#define TBAR_H     18
#define MENUBAR_H  20
#define ICON_PITCH 92
#define ICON0_X    36
#define ICON0_Y    44
#define NICONS     5
#define MAXWIN     6
#define DBLCLICK   30

enum { APP_SYSINFO = 0, APP_CLOCK, APP_FILES, APP_NOTEPAD, APP_MUSIC };

typedef struct {
    Boolean used;
    short   proc;
    Rect    bounds;
    const char *title;
} UnoWin;

/* ---- state ------------------------------------------------------------- */
static WindowPtr gWin;
static Rect   gScreen;
static UnoWin gWins[MAXWIN];
static short  gZ[MAXWIN];
static short  gZCount = 0;
static short  gSel = 0;
static long   gBootTicks;
static long   gLastSec = -1;
static short  gDblIcon = -1;
static long   gDblTick = 0;

static Boolean gDragging = false;
static short  gDragWin = -1;
static short  gDragDX, gDragDY;
static Rect   gDragOutline;
static Boolean gOutlineShown = false;

static const char *kIconNames[NICONS]  = { "Sys Info", "Clock", "Files", "Notepad", "Music" };
static const char *kWinTitles[NICONS]  = { "System Info", "Clock", "Files", "Notepad", "Music" };

/* default window bounds per app (fits the 512x342 mono screen) */
static const short kWinRect[NICONS][4] = {
    {  40,  50, 320, 170 },     /* SysInfo  */
    { 120,  80, 320, 180 },     /* Clock    */
    {  36,  40, 330, 270 },     /* Files    */
    {  56,  34, 484, 320 },     /* Notepad  */
    {  80,  60, 440, 230 },     /* Music    */
};

/* =========================================================================
 * Theme layer - the ONLY part that differs between the color and mono apps
 *  COLOR: the UnoDOS 4-colour palette literally.
 *  MONO : authentic 1-bit Mac - gray desktop, white windows, black ink.
 * ========================================================================= */
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

static void uno_fill(Rect *r, short c)
{
#if UNO_COLOR
    RGBForeColor(&kPalette[c]);
    PaintRect(r);
    RGBForeColor(&kBlack);
#else
    if (c == C_WHITE || c == C_BLUE) FillRect(r, &qd.white);
    else                             FillRect(r, &qd.black);
#endif
}

static void uno_box(Rect *r, short c)
{
#if UNO_COLOR
    RGBForeColor(&kPalette[c]);
    FrameRect(r);
    RGBForeColor(&kBlack);
#else
    (void)c;
    ForeColor(blackColor);
    FrameRect(r);
#endif
}

/* invert a rect (selection bars) - works on both targets */
static void uno_invert(Rect *r) { InvertRect(r); }

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
    TextMode(opaque ? srcCopy : srcOr);
    DrawText((Ptr)s, 0, len);
    TextMode(srcOr);
#endif
}

/* truncated text: draw at most maxw pixels wide */
static void text_at_max(short x, short y, const char *s, short fg, short maxw)
{
    short len = (short)strlen(s);
    while (len > 0 && TextWidth((Ptr)s, 0, len) > maxw) len--;
    if (len <= 0) return;
    MoveTo(x, y);
#if UNO_COLOR
    RGBForeColor(&kPalette[fg]);
    TextMode(srcOr);
    DrawText((Ptr)s, 0, len);
    RGBForeColor(&kBlack);
#else
    (void)fg;
    ForeColor(blackColor);
    TextMode(srcOr);
    DrawText((Ptr)s, 0, len);
#endif
}

/* =========================================================================
 * Small helpers
 * ========================================================================= */
static long now_secs(void) { return (TickCount() - gBootTicks) / 60; }

static void fmt_u(long v, char *out)
{
    char tmp[12]; int n = 0, i = 0;
    if (v <= 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n) out[i++] = tmp[--n];
    out[i] = 0;
}
static void put2(long v, char *out) { out[0]='0'+(v/10)%10; out[1]='0'+v%10; out[2]=0; }
static void cat(char *d, const char *s) { strcat(d, s); }

/* =========================================================================
 * App forward declarations
 * ========================================================================= */
static void draw_app_content(short proc, UnoWin *w);
static Boolean app_key(short proc, char ch, short code, Boolean cmd);
static void app_click(short proc, UnoWin *w, Point p);
static void app_close(short proc);
static void app_opened(short proc);
static UnoWin *find_app_window(short proc);
static void launch_app(short proc);
static void repaint_all(void);

/* =========================================================================
 * Window manager (PORT-SPEC SS2)
 * ========================================================================= */
static UnoWin *zwin(short z) { return &gWins[gZ[z]]; }

static short find_window_at(Point p)
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

static void draw_window(UnoWin *w)
{
    Rect r = w->bounds, tb, ct;
    ct = r; ct.top += TBAR_H; InsetRect(&ct, 1, 1); uno_fill(&ct, C_BLUE);
    tb = r; tb.bottom = tb.top + TBAR_H; uno_fill(&tb, C_WHITE);
    uno_box(&r, C_WHITE);
    { Rect sep = tb; sep.top = sep.bottom - 1;
#if UNO_COLOR
      RGBForeColor(&kPalette[C_BLUE]);
#else
      ForeColor(blackColor);
#endif
      MoveTo(sep.left, sep.top); LineTo(sep.right - 1, sep.top);
#if UNO_COLOR
      RGBForeColor(&kBlack);
#endif
    }
    text_at(r.left + 6, r.top + 13, w->title, C_BLUE, C_WHITE, true);
    text_at(r.right - 14, r.top + 13, "X", C_BLUE, C_WHITE, true);
    draw_app_content(w->proc, w);
}

static void draw_desktop(void);

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
    if (z == gZCount - 1) return;
    slot = gZ[z];
    for (i = z; i < gZCount - 1; i++) gZ[i] = gZ[i + 1];
    gZ[gZCount - 1] = slot;
    repaint_all();
}

static void close_window(short z)
{
    short i;
    app_close(gWins[gZ[z]].proc);
    gWins[gZ[z]].used = false;
    for (i = z; i < gZCount - 1; i++) gZ[i] = gZ[i + 1];
    gZCount--;
    repaint_all();
}

static UnoWin *find_app_window(short proc)
{
    short i;
    for (i = 0; i < gZCount; i++)
        if (gWins[gZ[i]].proc == proc) return &gWins[gZ[i]];
    return NULL;
}

static void launch_app(short proc)
{
    short i, slot = -1;
    for (i = 0; i < gZCount; i++)
        if (gWins[gZ[i]].proc == proc) { raise_window(i); return; }
    for (i = 0; i < MAXWIN; i++) if (!gWins[i].used) { slot = i; break; }
    if (slot < 0) return;

    gWins[slot].used  = true;
    gWins[slot].proc  = proc;
    gWins[slot].title = kWinTitles[proc];
    SetRect(&gWins[slot].bounds, kWinRect[proc][0], kWinRect[proc][1],
                                 kWinRect[proc][2], kWinRect[proc][3]);
    gZ[gZCount++] = slot;
    app_opened(proc);
    draw_window(&gWins[slot]);
}

/* =========================================================================
 * Files app - File Manager directory listing
 * ========================================================================= */
#define FMAX   24
#define FROWS  11
#define FROW_H 16

static unsigned char gFNames[FMAX][32];     /* Pascal strings */
static long    gFSizes[FMAX];
static Boolean gFIsDir[FMAX];
static long    gFDirIDs[FMAX];              /* dirID per entry (dirs only) */
static long    gFParID = 0;                 /* parent of the current dir */
static Boolean gFAtRoot = true;
static short   gFCount = 0, gFSel = 0, gFTop = 0;
static short   gFLastRow = -1;
static long    gFLastTick = 0;

static void files_refresh(void)
{
    CInfoPBRec cpb;
    short i;
    unsigned char scratch[64];

    /* where are we? ioFDirIndex = -1 describes the default dir itself
       (real dirID + parent). MFS / pre-HFS errors mean "flat" = root. */
    gFAtRoot = true;
    gFParID = 0;
    memset(&cpb, 0, sizeof(cpb));
    cpb.dirInfo.ioVRefNum = 0;
    cpb.dirInfo.ioFDirIndex = -1;
    cpb.dirInfo.ioDrDirID = 0;
    cpb.dirInfo.ioNamePtr = scratch;
    if (PBGetCatInfoSync(&cpb) == noErr && cpb.dirInfo.ioDrDirID > 2) {        /* fsRtDirID == 2 */
        gFAtRoot = false;
        gFParID = cpb.dirInfo.ioDrParID;
    }

    gFCount = 0;
    if (!gFAtRoot) {                        /* parent entry */
        gFNames[0][0] = 2; gFNames[0][1] = '.'; gFNames[0][2] = '.';
        gFIsDir[0] = true;
        gFSizes[0] = 0;
        gFDirIDs[0] = gFParID;
        gFCount = 1;
    }
    for (i = 1; gFCount < FMAX; i++) {
        OSErr err;
        memset(&cpb, 0, sizeof(cpb));
        cpb.dirInfo.ioVRefNum = 0;          /* default volume */
        cpb.dirInfo.ioFDirIndex = i;
        cpb.dirInfo.ioDrDirID = 0;          /* default directory */
        cpb.dirInfo.ioNamePtr = gFNames[gFCount];
        err = PBGetCatInfoSync(&cpb);
        if (err != noErr) break;
        if (cpb.hFileInfo.ioFlAttrib & 0x10) {      /* directory */
            gFIsDir[gFCount] = true;
            gFSizes[gFCount] = 0;
            gFDirIDs[gFCount] = cpb.dirInfo.ioDrDirID;
        } else {
            gFIsDir[gFCount] = false;
            gFSizes[gFCount] = cpb.hFileInfo.ioFlLgLen;
            gFDirIDs[gFCount] = 0;
        }
        gFCount++;
    }
    if (gFSel >= gFCount) gFSel = gFCount ? gFCount - 1 : 0;
    if (gFTop > gFSel) gFTop = gFSel;
}

static void files_enter_dir(long dirID)
{
    WDPBRec pb;
    memset(&pb, 0, sizeof(pb));
    pb.ioVRefNum = 0;                       /* default volume */
    pb.ioWDDirID = dirID;
    if (PBHSetVolSync(&pb) != noErr) return;
    gFSel = 0; gFTop = 0; gFLastRow = -1;
    files_refresh();
}

static void files_draw(UnoWin *w)
{
    Rect r = w->bounds, row;
    short x = r.left + 8, y0 = r.top + TBAR_H + 4, i;
    char line[64], num[16];

    text_at(x, y0 + 10, "Name", C_CYAN, C_BLUE, false);
    text_at(r.right - 80, y0 + 10, "Size", C_CYAN, C_BLUE, false);

    for (i = 0; i < FROWS; i++) {
        short fi = gFTop + i;
        short ry = y0 + 14 + i * FROW_H;
        Boolean sel;
        SetRect(&row, r.left + 2, ry, r.right - 2, ry + FROW_H);
        if (fi >= gFCount) break;
        sel = (fi == gFSel);
#if UNO_COLOR
        if (sel) uno_fill(&row, C_CYAN);    /* explicit palette selection bar
                                               (InvertRect is index-inversion
                                               in 8-bit - off-palette) */
#endif
        {
            short n = gFNames[fi][0]; if (n > 31) n = 31;
            memcpy(line, gFNames[fi] + 1, n); line[n] = 0;
        }
        text_at_max(x, ry + 12, line, sel ? C_BLUE : C_WHITE, r.right - r.left - 100);
        if (gFIsDir[fi]) {
            text_at(r.right - 80, ry + 12, "<DIR>", sel ? C_BLUE : C_MAG, C_BLUE, false);
        } else {
            fmt_u(gFSizes[fi], num);
            text_at(r.right - 80, ry + 12, num, sel ? C_BLUE : C_WHITE, C_BLUE, false);
        }
#if !UNO_COLOR
        if (sel) uno_invert(&row);          /* 1-bit invert is the classic look */
#endif
    }
    text_at(x, r.bottom - 6, "Enter: open/enter dir   R: refresh", C_CYAN, C_BLUE, false);
}

static void notepad_load_pascal(const unsigned char *pname);

static void files_open_sel(void)
{
    if (gFCount == 0) return;
    if (gFIsDir[gFSel]) {
        UnoWin *w = find_app_window(APP_FILES);
        files_enter_dir(gFDirIDs[gFSel]);
        if (w) draw_window(w);
        return;
    }
    notepad_load_pascal(gFNames[gFSel]);
    launch_app(APP_NOTEPAD);
}

static Boolean files_key(char ch, short code)
{
    UnoWin *w = find_app_window(APP_FILES);
    if (code == 0x7D || ch == 0x1F) {                   /* down */
        if (gFSel < gFCount - 1) gFSel++;
        if (gFSel >= gFTop + FROWS) gFTop = gFSel - FROWS + 1;
        if (w) draw_window(w);
        return true;
    }
    if (code == 0x7E || ch == 0x1E) {                   /* up */
        if (gFSel > 0) gFSel--;
        if (gFSel < gFTop) gFTop = gFSel;
        if (w) draw_window(w);
        return true;
    }
    if (ch == 0x0D || ch == 0x03) { files_open_sel(); return true; }
    if (ch == 'r' || ch == 'R') {
        files_refresh();
        if (w) draw_window(w);
        return true;
    }
    return false;
}

static void files_click(UnoWin *w, Point p)
{
    short y0 = w->bounds.top + TBAR_H + 18;
    short row = (p.v - y0) / FROW_H;
    long t = TickCount();
    if (row < 0 || gFTop + row >= gFCount) return;
    gFSel = gFTop + row;
    if (row == gFLastRow && t - gFLastTick <= DBLCLICK) {
        gFLastRow = -1;
        files_open_sel();
        return;
    }
    gFLastRow = row; gFLastTick = t;
    draw_window(w);
}

/* =========================================================================
 * Notepad app - text editor with the live status bar (audit rule)
 * ========================================================================= */
#define NBUF   4096
#define NLINE_H 14

static char    gNBuf[NBUF];
static short   gNLen = 0, gNCaret = 0, gNTop = 0;   /* gNTop = first line shown */
static Boolean gNDirty = false;
static unsigned char gNFile[32] = "\pUNTITLED.TXT";

static void notepad_caret_linecol(short *line, short *col)
{
    short i, l = 0, c = 0;
    for (i = 0; i < gNCaret; i++) {
        if (gNBuf[i] == '\r') { l++; c = 0; } else c++;
    }
    *line = l; *col = c;
}

static short notepad_line_start(short line)
{
    short i, l = 0;
    if (line <= 0) return 0;
    for (i = 0; i < gNLen; i++)
        if (gNBuf[i] == '\r' && ++l == line) return i + 1;
    return gNLen;
}

static void notepad_draw(UnoWin *w)
{
    Rect r = w->bounds, ct;
    short rows = (r.bottom - r.top - TBAR_H - 22) / NLINE_H;
    short x = r.left + 5, y = r.top + TBAR_H + 12;
    short line, col, ln = 0, i = 0, drawn = 0;
    char st[80], num[12];

    /* content backdrop (re-clear: editor repaints fully per edit) */
    ct = r; ct.top += TBAR_H; InsetRect(&ct, 1, 1);
    ct.bottom -= 14;
    uno_fill(&ct, C_BLUE);

    /* visible lines */
    i = notepad_line_start(gNTop);
    ln = gNTop;
    while (i <= gNLen && drawn < rows) {
        short e = i;
        while (e < gNLen && gNBuf[e] != '\r') e++;
        if (e > i) {
            char tmp; short maxw = r.right - r.left - 12;
            short len = e - i;
            /* truncate to window width */
            while (len > 0 && TextWidth((Ptr)gNBuf + i, 0, len) > maxw) len--;
            tmp = gNBuf[i + len];
            MoveTo(x, y + drawn * NLINE_H);
#if UNO_COLOR
            RGBForeColor(&kPalette[C_WHITE]);
#else
            ForeColor(blackColor);
#endif
            TextMode(srcOr);
            DrawText((Ptr)gNBuf + i, 0, len);
#if UNO_COLOR
            RGBForeColor(&kBlack);
#endif
            (void)tmp;
        }
        /* caret on this line? */
        notepad_caret_linecol(&line, &col);
        if (line == ln) {
            short cw = TextWidth((Ptr)gNBuf + i, 0, gNCaret - i);
            Rect cr;
            SetRect(&cr, x + cw, y + drawn * NLINE_H - 10, x + cw + 2, y + drawn * NLINE_H + 2);
            uno_fill(&cr, C_CYAN);
        }
        if (e >= gNLen) break;
        i = e + 1; ln++; drawn++;
    }

    /* status bar - LIVE on every keystroke (x86 audit parity) */
    {
        Rect sb = r; sb.top = sb.bottom - 14; InsetRect(&sb, 1, 1);
        uno_fill(&sb, C_WHITE);
        notepad_caret_linecol(&line, &col);
        st[0] = 0;
        cat(st, "Ln "); fmt_u(line + 1, num); cat(st, num);
        cat(st, "  Co "); fmt_u(col + 1, num); cat(st, num);
        cat(st, "  "); fmt_u(gNLen, num); cat(st, num); cat(st, " B");
        if (gNDirty) cat(st, " *");
        cat(st, "   Cmd-S: save");
        text_at(r.left + 6, r.bottom - 4, st, C_BLUE, C_WHITE, true);
    }
}

static void notepad_scroll_to_caret(UnoWin *w)
{
    short line, col;
    short rows = (w->bounds.bottom - w->bounds.top - TBAR_H - 22) / NLINE_H;
    notepad_caret_linecol(&line, &col);
    if (line < gNTop) gNTop = line;
    if (line >= gNTop + rows) gNTop = line - rows + 1;
}

static void notepad_load_pascal(const unsigned char *pname)
{
    short ref;
    long count = NBUF;
    OSErr err;
    memcpy(gNFile, pname, pname[0] + 1);
    gNLen = 0; gNCaret = 0; gNTop = 0; gNDirty = false;
    err = FSOpen((ConstStr255Param)gNFile, 0, &ref);
    if (err == noErr) {
        err = FSRead(ref, &count, gNBuf);
        if (err == noErr || err == eofErr) {
            short i;
            gNLen = (short)count;
            /* normalize LF -> CR for display */
            for (i = 0; i < gNLen; i++)
                if (gNBuf[i] == '\n') gNBuf[i] = '\r';
        }
        FSClose(ref);
    }
}

static void notepad_save(void)
{
    short ref;
    long count = gNLen;
    OSErr err;
    FSDelete((ConstStr255Param)gNFile, 0);
    err = Create((ConstStr255Param)gNFile, 0, 'UNOD', 'TEXT');
    if (err != noErr && err != dupFNErr) return;
    err = FSOpen((ConstStr255Param)gNFile, 0, &ref);
    if (err != noErr) return;
    FSWrite(ref, &count, gNBuf);
    FSClose(ref);
    FlushVol(NULL, 0);
    gNDirty = false;
}

static Boolean notepad_key(char ch, short code, Boolean cmd)
{
    UnoWin *w = find_app_window(APP_NOTEPAD);
    if (cmd) {
        if (ch == 's' || ch == 'S') { notepad_save(); if (w) draw_window(w); return true; }
        return false;
    }
    if (code == 0x7B || ch == 0x1C) {                   /* left */
        if (gNCaret > 0) gNCaret--;
    } else if (code == 0x7C || ch == 0x1D) {            /* right */
        if (gNCaret < gNLen) gNCaret++;
    } else if (code == 0x7E || ch == 0x1E) {            /* up */
        short line, col, s;
        notepad_caret_linecol(&line, &col);
        if (line > 0) {
            short prev = notepad_line_start(line - 1);
            short prevLen = notepad_line_start(line) - 1 - prev;
            s = col < prevLen ? col : prevLen;
            gNCaret = prev + (s < 0 ? 0 : s);
        }
    } else if (code == 0x7D || ch == 0x1F) {            /* down */
        short line, col;
        notepad_caret_linecol(&line, &col);
        {
            short next = notepad_line_start(line + 1);
            if (next <= gNLen) {
                short e = next;
                short nl;
                while (e < gNLen && gNBuf[e] != '\r') e++;
                nl = e - next;
                gNCaret = next + (col < nl ? col : nl);
            }
        }
    } else if (ch == 0x08 || ch == 0x7F) {              /* backspace/del */
        if (gNCaret > 0) {
            memmove(gNBuf + gNCaret - 1, gNBuf + gNCaret, gNLen - gNCaret);
            gNCaret--; gNLen--; gNDirty = true;
        }
    } else if (ch == 0x0D || ch == 0x03 || ch >= 32) {  /* insert */
        if (gNLen < NBUF - 1) {
            char c = (ch == 0x03) ? 0x0D : ch;
            memmove(gNBuf + gNCaret + 1, gNBuf + gNCaret, gNLen - gNCaret);
            gNBuf[gNCaret++] = c; gNLen++; gNDirty = true;
        }
    } else {
        return false;
    }
    if (w) { notepad_scroll_to_caret(w); draw_window(w); }
    return true;
}

/* =========================================================================
 * Music app - Canon in D on the Sound Manager square-wave synth
 * (same arrangement as the x86 apps/music.asm)
 * ========================================================================= */
#define QN 30                       /* quarter note, ticks (60 Hz) */
#define EN 15                       /* eighth note */

typedef struct { unsigned char midi; unsigned char dur; } Note;
/* MIDI: C4=60 D=62 E=64 F=65 G=67 A=69 B=71 C5=72 D5=74 E5=76 */
static const Note kTune[] = {
    {72,QN},{71,QN},{69,QN},{67,QN},          /* C5 B4 A4 G4 */
    {65,QN},{64,QN},{65,QN},{67,QN},          /* F4 E4 F4 G4 */
    {72,EN},{76,EN},{71,EN},{74,EN},          /* eighth arpeggios */
    {69,EN},{72,EN},{67,EN},{71,EN},
    {65,EN},{69,EN},{64,EN},{67,EN},
    {65,EN},{69,EN},{67,EN},{71,EN},
};
#define NTUNE (short)(sizeof(kTune)/sizeof(kTune[0]))

static SndChannelPtr gSnd = NULL;
static Boolean gPlaying = false;
static short   gNoteIx = 0;
static long    gNoteEnd = 0;

static void music_open_chan(void)
{
    if (gSnd) return;
    if (SndNewChannel(&gSnd, noteSynth, 0, NULL)   /* = squareWaveSynth */ != noErr)
        gSnd = NULL;                /* no sound HW/emulation: visual only */
}

static void music_note_on(short midi, short durTicks)
{
    SndCommand c;
    if (!gSnd) return;
    c.cmd = noteCmd;                        /* = freqDurationCmd */
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

static void music_draw(UnoWin *w)
{
    Rect r = w->bounds;
    short x0 = r.left + 12, y;
    short staffTop = r.top + TBAR_H + 28;
    short i;
    Rect ct = r;

    ct.top += TBAR_H; InsetRect(&ct, 1, 1);
    uno_fill(&ct, C_BLUE);

    text_at(r.left + 8, r.top + TBAR_H + 14, "Canon in D  (Pachelbel)", C_WHITE, C_BLUE, false);

    /* staff: 5 lines */
    for (i = 0; i < 5; i++) {
        y = staffTop + 14 + i * 8;
#if UNO_COLOR
        RGBForeColor(&kPalette[C_WHITE]);
#else
        ForeColor(blackColor);
#endif
        MoveTo(x0, y); LineTo(r.right - 12, y);
#if UNO_COLOR
        RGBForeColor(&kBlack);
#endif
    }
    /* notes: position by pitch, highlight the playing one */
    for (i = 0; i < NTUNE; i++) {
        Rect nr;
        short nx = x0 + 4 + i * ((r.right - r.left - 32) / NTUNE);
        short ny = staffTop + 46 - (kTune[i].midi - 60) * 2;
        SetRect(&nr, nx, ny - 3, nx + 6, ny + 3);
        if (gPlaying && i == gNoteIx) uno_fill(&nr, C_MAG);
        else                          uno_fill(&nr, C_CYAN);
    }
    text_at(r.left + 8, r.bottom - 8,
            gPlaying ? "Space: stop" : "Space: play", C_CYAN, C_BLUE, false);
}

static void music_start(void)
{
    music_open_chan();
    gPlaying = true;
    gNoteIx = 0;
    gNoteEnd = TickCount() + kTune[0].dur;
    music_note_on(kTune[0].midi, kTune[0].dur);
}

static void music_stop(void)
{
    gPlaying = false;
    music_quiet();
}

static void music_tick(void)
{
    UnoWin *w;
    if (!gPlaying) return;
    if (TickCount() < gNoteEnd) return;
    gNoteIx++;
    if (gNoteIx >= NTUNE) gNoteIx = 0;      /* loop, like the x86 app */
    gNoteEnd = TickCount() + kTune[gNoteIx].dur;
    music_note_on(kTune[gNoteIx].midi, kTune[gNoteIx].dur);
    w = find_app_window(APP_MUSIC);
    if (w && zwin(gZCount - 1) == w) draw_window(w);    /* topmost-only refresh */
}

static Boolean music_key(char ch, short code)
{
    UnoWin *w;
    (void)code;
    if (ch == ' ') {
        if (gPlaying) music_stop(); else music_start();
        w = find_app_window(APP_MUSIC);
        if (w) draw_window(w);
        return true;
    }
    return false;
}

/* =========================================================================
 * SysInfo + Clock (milestone 1 apps)
 * ========================================================================= */
static void sysinfo_draw(UnoWin *w)
{
    short x = w->bounds.left + 8;
    short y = w->bounds.top + TBAR_H + 14;
    char num[16], line[24];
    text_at(x, y, "Video", C_WHITE, C_BLUE, false);
#if UNO_COLOR
    text_at(x + 80, y,      "Color QuickDraw", C_CYAN, C_BLUE, false);
    text_at(x, y + 16,      "System", C_WHITE, C_BLUE, false);
    text_at(x + 80, y + 16, "7.x (Mac II+)", C_CYAN, C_BLUE, false);
#else
    text_at(x + 80, y,      "1-bit QuickDraw", C_WHITE, C_BLUE, false);
    text_at(x, y + 16,      "System", C_WHITE, C_BLUE, false);
    text_at(x + 80, y + 16, "1-6 (68000)", C_WHITE, C_BLUE, false);
#endif
    text_at(x, y + 32, "Uptime", C_WHITE, C_BLUE, false);
    fmt_u(now_secs(), num); strcpy(line, num); cat(line, "s   ");
    text_at(x + 80, y + 32, line, C_CYAN, C_BLUE, true);
    text_at(x, y + 54, "UnoDOS/Mac  Milestone 2", C_MAG, C_BLUE, false);
}

static void clock_draw(UnoWin *w)
{
    long s = now_secs();
    char buf[12];
    short cx, cy;
    put2(s / 3600, buf);          buf[2] = ':';
    put2((s / 60) % 60, buf + 3); buf[5] = ':';
    put2(s % 60, buf + 6);        buf[8] = 0;
    text_at(w->bounds.left + 8, w->bounds.top + TBAR_H + 14, "Uptime", C_WHITE, C_BLUE, false);
    cx = w->bounds.left + (w->bounds.right - w->bounds.left) / 2 - 28;
    cy = w->bounds.top + (w->bounds.bottom - w->bounds.top) / 2 + 8;
    text_at(cx, cy, buf, C_CYAN, C_BLUE, true);
}

/* =========================================================================
 * App dispatch
 * ========================================================================= */
static void draw_app_content(short proc, UnoWin *w)
{
    switch (proc) {
    case APP_SYSINFO: sysinfo_draw(w); break;
    case APP_CLOCK:   clock_draw(w);   break;
    case APP_FILES:   files_draw(w);   break;
    case APP_NOTEPAD: notepad_draw(w); break;
    case APP_MUSIC:   music_draw(w);   break;
    }
}

static Boolean app_key(short proc, char ch, short code, Boolean cmd)
{
    switch (proc) {
    case APP_FILES:   if (!cmd) return files_key(ch, code); break;
    case APP_NOTEPAD: return notepad_key(ch, code, cmd);
    case APP_MUSIC:   if (!cmd) return music_key(ch, code); break;
    }
    return false;
}

static void app_click(short proc, UnoWin *w, Point p)
{
    if (proc == APP_FILES) files_click(w, p);
}

static void app_opened(short proc)
{
    if (proc == APP_FILES) files_refresh();
    if (proc == APP_MUSIC) music_open_chan();
}

static void app_close(short proc)
{
    if (proc == APP_MUSIC) {
        music_stop();
        if (gSnd) { SndDisposeChannel(gSnd, true); gSnd = NULL; }
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
    desktop_bg(&cell);
    switch (i) {
    case APP_SYSINFO:                       /* monitor */
        SetRect(&g, x, ICON0_Y, x + 18, ICON0_Y + 13);
        uno_box(&g, C_CYAN);
        { Rect inr = g; InsetRect(&inr, 2, 2); uno_fill(&inr, C_CYAN); }
        SetRect(&g, x + 6, ICON0_Y + 13, x + 12, ICON0_Y + 16); uno_box(&g, C_CYAN);
        break;
    case APP_CLOCK:                         /* clock face */
        SetRect(&g, x, ICON0_Y, x + 16, ICON0_Y + 16);
#if UNO_COLOR
        RGBForeColor(&kPalette[C_CYAN]);
#else
        ForeColor(blackColor);
#endif
        FrameOval(&g);
        MoveTo(x + 8, ICON0_Y + 8); LineTo(x + 8, ICON0_Y + 3);
        MoveTo(x + 8, ICON0_Y + 8); LineTo(x + 12, ICON0_Y + 8);
#if UNO_COLOR
        RGBForeColor(&kBlack);
#endif
        break;
    case APP_FILES:                         /* folder */
        SetRect(&g, x, ICON0_Y + 3, x + 18, ICON0_Y + 15);
        uno_fill(&g, C_CYAN);
        SetRect(&g, x, ICON0_Y, x + 9, ICON0_Y + 5);
        uno_fill(&g, C_CYAN);
        SetRect(&g, x, ICON0_Y + 3, x + 18, ICON0_Y + 15);
        uno_box(&g, C_WHITE);
        break;
    case APP_NOTEPAD:                       /* page with lines */
        SetRect(&g, x + 1, ICON0_Y, x + 15, ICON0_Y + 17);
        uno_fill(&g, C_WHITE);
        uno_box(&g, C_CYAN);
        {
            short ly;
            for (ly = ICON0_Y + 3; ly < ICON0_Y + 15; ly += 3) {
#if UNO_COLOR
                RGBForeColor(&kPalette[C_BLUE]);
#else
                ForeColor(blackColor);
#endif
                MoveTo(x + 3, ly); LineTo(x + 12, ly);
#if UNO_COLOR
                RGBForeColor(&kBlack);
#endif
            }
        }
        break;
    case APP_MUSIC:                         /* eighth note */
        SetRect(&g, x + 2, ICON0_Y + 11, x + 8, ICON0_Y + 17);
        uno_fill(&g, C_CYAN);
#if UNO_COLOR
        RGBForeColor(&kPalette[C_CYAN]);
#else
        ForeColor(blackColor);
#endif
        MoveTo(x + 7, ICON0_Y + 13); LineTo(x + 7, ICON0_Y);
        LineTo(x + 14, ICON0_Y + 3);
#if UNO_COLOR
        RGBForeColor(&kBlack);
#endif
        break;
    }
    text_at(x - 8, ICON0_Y + 28, kIconNames[i], C_WHITE, C_BLUE, true);
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
    text_at(gScreen.left + 6, gScreen.bottom - 8, "UnoDOS/Mac 7  v0.2.0", C_WHITE, C_BLUE, true);
#else
    text_at(gScreen.left + 6, gScreen.bottom - 8, "UnoDOS/Mac Classic  v0.2.0", C_WHITE, C_BLUE, true);
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
 * Drag (XOR outline, clamped - PORT-SPEC SS2)
 * ========================================================================= */
static void xor_outline(Rect *r)
{
    PenMode(patXor);
    PenPat(&qd.gray);
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
 * Input routing - focused (topmost) window owns the keyboard
 * ========================================================================= */
static void on_mouse_down(Point p)
{
    short z = find_window_at(p);
    if (z >= 0) {
        UnoWin *w = zwin(z);
        if (p.v < w->bounds.top + TBAR_H) {
            if (p.h >= w->bounds.right - 14) { close_window(z); return; }
            raise_window(z);
            gDragWin = gZ[gZCount - 1];
            gDragDX = p.h - gWins[gDragWin].bounds.left;
            gDragDY = p.v - gWins[gDragWin].bounds.top;
            gDragging = true; gOutlineShown = false;
        } else {
            if (z != gZCount - 1) { raise_window(z); return; }
            app_click(zwin(gZCount - 1)->proc, zwin(gZCount - 1), p);
        }
        return;
    }
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

static void on_key(char ch, short code, Boolean cmd)
{
    /* focused window gets first refusal (PORT-SPEC SS3) */
    if (gZCount > 0) {
        if (app_key(zwin(gZCount - 1)->proc, ch, code, cmd))
            return;
        if (ch == 0x1B) { close_window(gZCount - 1); return; }  /* ESC */
        return;
    }
    /* desktop navigation */
    if (code == 0x7C || ch == 0x1D)      select_icon((gSel + 1) % NICONS);
    else if (code == 0x7B || ch == 0x1C) select_icon((gSel + NICONS - 1) % NICONS);
    else if (ch == 0x0D || ch == 0x03)   launch_app(gSel);
}

static void app_secondly(void)
{
    long s = now_secs();
    if (s == gLastSec) return;
    gLastSec = s;
    if (gZCount > 0) {
        UnoWin *w = zwin(gZCount - 1);
        if (w->proc == APP_SYSINFO || w->proc == APP_CLOCK)
            draw_window(w);
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

    r = gScreen;
#if UNO_COLOR
    gWin = NewCWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1L, false, 0);
#else
    gWin = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1L, false, 0);
#endif
    SetPort(gWin);
    TextFont(0);
    TextSize(12);

    gBootTicks = TickCount();
    gSel = 0;
    draw_desktop();

#ifdef UNO_AUTOTEST_FILES
    /* Files-focused variant: enter the first real subdirectory through the
       normal open path, so the screenshot shows the subdir listing with the
       ".." parent entry (proves PBGetCatInfo dirID navigation). */
    launch_app(APP_FILES);
    {
        short i;
        for (i = 0; i < gFCount; i++) {
            if (gFIsDir[i] && (gFAtRoot || gFDirIDs[i] != gFParID)) {
                gFSel = i;
                files_open_sel();
                break;
            }
        }
    }
#endif
#ifdef UNO_AUTOTEST
    /* Auto-launch the app stack for screenshot verification without
       host->guest input injection. Notepad gets demo text. */
    launch_app(APP_MUSIC);
    music_start();
    launch_app(APP_FILES);
    {
        const char *demo = "UnoDOS/Mac milestone 2\rThe quick brown fox\rjumps over the lazy dog.";
        gNLen = (short)strlen(demo);
        memcpy(gNBuf, demo, gNLen);
        gNCaret = gNLen;
        gNDirty = true;
    }
    launch_app(APP_NOTEPAD);
#endif

    for (;;) {
        Boolean got = GetNextEvent(everyEvent, &e);
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
                Boolean cmd = (e.modifiers & cmdKey) != 0;
                if (cmd && (ch == 'q' || ch == 'Q')) return 0;
                on_key(ch, code, cmd);
                break;
            }
            }
        }
        if (gDragging) {
            Point p;
            GetMouse(&p);
            drag_update(p, StillDown());
        }
        music_tick();
        app_secondly();
    }
    return 0;
}

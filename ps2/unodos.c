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

/* PS2 port: the Mac Toolbox surface is re-implemented over the software
   framebuffer fb.* by mac_compat.* (HANDOFF SS1). This single include replaces
   the dozen <Quickdraw.h>/<Windows.h>/... headers; `qd` lives in the shim. */
#include "mac_compat.h"
#include <string.h>

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

/* 8 preset palettes shared with the other ports. Slot roles: desktop,
   accent (cyan-role), accent2 (magenta-role), text (white-role).
   Preset 1 "Classic VGA" is the PC palette. */
#define NTHEMES 8
static const char *kThemeNames[NTHEMES] = {
    "Classic VGA", "Midnight", "Forest", "Sunset",
    "Ocean", "Slate", "Candy", "Amber"
};
static const RGBColor kThemes[NTHEMES][4] = {
  {{0x0000,0x0000,0xAAAA},{0x0000,0xAAAA,0xAAAA},{0xAAAA,0x0000,0xAAAA},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x0000,0x0000,0x0000},{0x5555,0x5555,0xFFFF},{0xAAAA,0xAAAA,0xAAAA},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x0000,0x5555,0x0000},{0x5555,0xAAAA,0x5555},{0xFFFF,0xFFFF,0x5555},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x5555,0x0000,0x0000},{0xFFFF,0x5555,0x5555},{0xFFFF,0xAAAA,0x0000},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x0000,0x0000,0x5555},{0x0000,0x8888,0xAAAA},{0x5555,0xFFFF,0xFFFF},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x3333,0x3333,0x4444},{0x8888,0x8888,0xAAAA},{0xCCCC,0xCCCC,0xDDDD},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x5555,0x0000,0x5555},{0xFFFF,0x5555,0xFFFF},{0x5555,0xFFFF,0xFFFF},{0xFFFF,0xFFFF,0xFFFF}},
  {{0x0000,0x0000,0x0000},{0xAAAA,0x5555,0x0000},{0xFFFF,0xAAAA,0x0000},{0xFFFF,0xFFFF,0xFFFF}},
};
static short gTSel = 0, gTSlot = 0;
#endif

/* ---- layout ------------------------------------------------------------- */
#define TBAR_H     18
#define MENUBAR_H  20
#define ICON_PITCH 92
#define ICON0_X    36
#define ICON0_Y    44
#define NAPPS      11
#if UNO_COLOR
#define NICONS     11               /* Theme icon is color-only */
#define ICONS_ROW  6                /* desktop icon grid, 640px screen */
#else
#define NICONS     10
#define ICONS_ROW  5                /* 512px screen */
#endif
#define ICON_ROW_H 72
#define MAXWIN     6
#define DBLCLICK   30

enum { APP_SYSINFO = 0, APP_CLOCK, APP_FILES, APP_NOTEPAD, APP_MUSIC,
       APP_DOSTRIS, APP_OUTLAST, APP_PACMAN, APP_TRACKER, APP_PAINT,
       APP_THEME };

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

static const char *kIconNames[NAPPS]  = { "Sys Info", "Clock", "Files", "Notepad", "Music", "Dostris", "OutLast", "Pac-Man", "Tracker", "Paint", "Theme" };
static const char *kWinTitles[NAPPS]  = { "System Info", "Clock", "Files", "Notepad", "Music", "Dostris", "OutLast", "Pac-Man", "Tracker", "Paint", "Theme" };

/* default window bounds per app (fits the 512x342 mono screen) */
static const short kWinRect[NAPPS][4] = {
    {  40,  50, 320, 170 },     /* SysInfo  */
    { 120,  80, 320, 180 },     /* Clock    */
    {  36,  40, 330, 270 },     /* Files    */
    {  56,  34, 484, 320 },     /* Notepad  */
    {  80,  60, 440, 230 },     /* Music    */
    {  20,  10, 330, 388 },     /* Dostris  */
    {  70,  40, 562, 384 },     /* OutLast  */
    {  70,  30, 404, 262 },     /* Pac-Man  */
    {  26,  30, 486, 326 },     /* Tracker  */
    {  14,  24, 498, 334 },     /* Paint    */
    { 150,  56, 430, 300 },     /* Theme    */
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
#if UNO_COLOR
static void theme_draw(UnoWin *w);
static Boolean theme_key(char ch, short code);
#endif
static void dostris_draw(UnoWin *w);
static Boolean dostris_key(char ch, short code);
static void dostris_tick(void);
static void outlast_draw(UnoWin *w);
static Boolean outlast_key(char ch, short code);
static void outlast_tick(void);
static void pacman_draw(UnoWin *w);
static Boolean pacman_key(char ch, short code);
static void pacman_tick(void);

static void gm_stop(void);
static void sched_init(void);
static void task_spawn(short slot);
static void task_kill(short slot);
static void task_post(short slot, char type, short d1, short d2, Boolean cmd);
static void task_post_key(short slot, short d1, short d2, Boolean cmd);
static void task_yield(void);
static void post_ticks(void);

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
    Boolean active = (gZCount > 0 && zwin(gZCount - 1) == w);
    /* System 7 chrome: 1px-offset drop shadow right + bottom */
    { Rect sh = r;
      sh.left = sh.right; sh.right += 1; sh.top += 2;
#if UNO_COLOR
      PaintRect(&sh);
#else
      FillRect(&sh, &qd.black);
#endif
      sh = r; sh.top = sh.bottom; sh.bottom += 1; sh.left += 2;
#if UNO_COLOR
      PaintRect(&sh);
#else
      FillRect(&sh, &qd.black);
#endif
    }
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
    /* pinstripes when active (= topmost), classic System 7 title bar */
    if (active) {
        short yy;
#if UNO_COLOR
        RGBForeColor(&kPalette[C_BLUE]);
#else
        ForeColor(blackColor);
#endif
        for (yy = tb.top + 3; yy <= tb.bottom - 4; yy += 3) {
            MoveTo(tb.left + 4, yy); LineTo(tb.right - 5, yy);
        }
#if UNO_COLOR
        RGBForeColor(&kBlack);
#endif
    }
    /* close box on a white patch (hit region unchanged: right - 14) */
    { Rect cb;
      SetRect(&cb, r.right - 22, tb.top + 1, r.right - 2, tb.bottom - 2);
      uno_fill(&cb, C_WHITE);
      SetRect(&cb, r.right - 18, r.top + 4, r.right - 7, r.top + 15);
#if UNO_COLOR
      RGBForeColor(&kPalette[C_BLUE]); FrameRect(&cb); RGBForeColor(&kBlack);
#else
      FrameRect(&cb);
#endif
    }
    /* centered title on a white patch */
    { short len = 0; const char *p = w->title;
      while (*p++) len++;
      { short tw = TextWidth((Ptr)w->title, 0, len);
        short tx = (short)((r.left + r.right - tw) / 2);
        Rect tp;
        SetRect(&tp, tx - 6, tb.top + 1, tx + tw + 6, tb.bottom - 2);
        uno_fill(&tp, C_WHITE);
        text_at(tx, r.top + 13, w->title, C_BLUE, C_WHITE, true);
      }
    }
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
    short i, slot = gZ[z];
    app_close(gWins[slot].proc);
    gWins[slot].used = false;
    task_kill(slot);                /* free the app task (milestone 3) */
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
    task_spawn(slot);               /* the app gets a task (milestone 3) */
    app_opened(proc);
    if (gZCount > 1) repaint_all(); /* the old topmost loses its stripes */
    else             draw_window(&gWins[slot]);
}

/* =========================================================================
 * PC-compatible floppy: FAT12 read/write (the Mac edition of the Amiga
 * port's fat12.i, in C). The filesystem core runs over an injectable
 * 512-byte block device:
 *
 *   - .Sony raw driver (refNum -5, drive 1): a 1.44MB MFM PC disk in a
 *     SuperDrive, addressed by absolute sector - real-hardware path
 *     (Executor has no floppy; classic Macs without an FDHD drive
 *     cannot read MFM at all).
 *   - RAM image fallback: a small FAT12 volume formatted in memory by
 *     the same code - the emulator/CI vehicle, byte-compatible with
 *     mkfat.py / mtools images, so the whole mount/list/read/write
 *     stack is verified under Executor.
 *
 * Files 'v' cycles HFS <-> PC floppy; Enter opens FAT files in Notepad
 * and Cmd-S writes back to the volume the file came from. The FAT is
 * cached whole (4.5KB for 1.44MB) and flushed to both copies on write,
 * like the Amiga core.
 * ========================================================================= */
#define FAT_LIST_MAX 16
#define FAT_MAX_IO   NBUF           /* biggest single file we move */

typedef Boolean (*FatBlkFn)(Boolean writeOp, short lba, unsigned char *buf);

static FatBlkFn gFatDev = NULL;     /* the injected block device */
static Boolean  gFatMounted = false;
static short    gFatSecPerClus, gFatRsvd, gFatNFats, gFatRootEnts;
static short    gFatFatSz, gFatRootStart, gFatDataStart, gFatTotSec;
static unsigned char *gFatCache = NULL;     /* whole FAT, first copy */
static Boolean  gFatDirty = false;
static unsigned char gFatSec[512];          /* shared sector buffer */

static unsigned char gFatNames[FAT_LIST_MAX][13];  /* C strings "NAME.EXT" */
static long     gFatSizes[FAT_LIST_MAX];
static short    gFatCount = 0;

/* ---- block devices ------------------------------------------------------ */
static Boolean fat_dev_sony(Boolean writeOp, short lba, unsigned char *buf)
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof(pb));
    pb.ioParam.ioRefNum    = -5;            /* .Sony */
    pb.ioParam.ioVRefNum   = 1;             /* internal drive */
    pb.ioParam.ioBuffer    = (Ptr)buf;
    pb.ioParam.ioReqCount  = 512;
    pb.ioParam.ioPosMode   = fsFromStart;
    pb.ioParam.ioPosOffset = (long)lba * 512;
    if (writeOp) {
        if (PBWriteSync(&pb) != noErr) return false;
    } else {
        if (PBReadSync(&pb) != noErr) return false;
    }
    return pb.ioParam.ioActCount == 512;
}

/* RAM image: 64 sectors - reserved 1, FAT 1 x2, root 1 (16 entries),
   data 60. Small, but a real FAT12 volume (mtools-readable layout). */
#define FATRAM_SECS 64
static unsigned char *gFatRam = NULL;

static Boolean fat_dev_ram(Boolean writeOp, short lba, unsigned char *buf)
{
    if (!gFatRam || lba < 0 || lba >= FATRAM_SECS) return false;
    if (writeOp) memcpy(gFatRam + (long)lba * 512, buf, 512);
    else         memcpy(buf, gFatRam + (long)lba * 512, 512);
    return true;
}

static unsigned short fat_rd16(const unsigned char *p) /* little-endian */
{
    return (unsigned short)(p[0] | (p[1] << 8));
}
static unsigned long fat_rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static void fat_wr16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)(v >> 8);
}
static void fat_wr32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);        p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF); p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* fat_ram_format - lay down a fresh mini FAT12 volume in the RAM image */
static void fat_ram_format(void)
{
    unsigned char *b = gFatRam;
    memset(b, 0, (long)FATRAM_SECS * 512);
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;          /* jmp + nop */
    memcpy(b + 3, "UNODOS  ", 8);                   /* OEM */
    fat_wr16(b + 11, 512);                          /* bytes/sector */
    b[13] = 1;                                      /* sectors/cluster */
    fat_wr16(b + 14, 1);                            /* reserved */
    b[16] = 2;                                      /* FAT copies */
    fat_wr16(b + 17, 16);                           /* root entries */
    fat_wr16(b + 19, FATRAM_SECS);                  /* total sectors */
    b[21] = 0xF0;                                   /* media */
    fat_wr16(b + 22, 1);                            /* FAT size */
    b[510] = 0x55; b[511] = 0xAA;
    /* FAT[0]/FAT[1] reserved marks, both copies (sectors 1 and 2) */
    b[512] = 0xF0; b[513] = 0xFF; b[514] = 0xFF;
    b[1024] = 0xF0; b[1025] = 0xFF; b[1026] = 0xFF;
}

/* ---- mount -------------------------------------------------------------- */
static Boolean fat12_mount_dev(FatBlkFn dev)
{
    if (!dev(false, 0, gFatSec)) return false;
    if (gFatSec[510] != 0x55 || gFatSec[511] != 0xAA) return false;
    if (fat_rd16(gFatSec + 11) != 512) return false;
    gFatSecPerClus = gFatSec[13];
    gFatRsvd       = (short)fat_rd16(gFatSec + 14);
    gFatNFats      = gFatSec[16];
    gFatRootEnts   = (short)fat_rd16(gFatSec + 17);
    gFatTotSec     = (short)fat_rd16(gFatSec + 19);
    gFatFatSz      = (short)fat_rd16(gFatSec + 22);
    if (!gFatSecPerClus || !gFatFatSz || !gFatRootEnts) return false;
    gFatRootStart  = (short)(gFatRsvd + gFatNFats * gFatFatSz);
    gFatDataStart  = (short)(gFatRootStart + (gFatRootEnts * 32 + 511) / 512);
    /* cache the first FAT copy whole */
    if (gFatCache) { DisposePtr((Ptr)gFatCache); gFatCache = NULL; }
    gFatCache = (unsigned char *)NewPtr((long)gFatFatSz * 512);
    if (!gFatCache) return false;
    {
        short s;
        for (s = 0; s < gFatFatSz; s++)
            if (!dev(false, (short)(gFatRsvd + s), gFatCache + (long)s * 512))
                return false;
    }
    gFatDev = dev;
    gFatDirty = false;
    gFatMounted = true;
    return true;
}

/* fat12_mount - the real drive first; in test builds fall back to the
   RAM image (formatted on first use) so Executor exercises everything */
static Boolean fat12_mount(void)
{
    if (gFatMounted) return true;
    if (fat12_mount_dev(fat_dev_sony)) return true;
#if defined(UNO_AUTOTEST) || defined(UNO_AUTOTEST_FAT12)
    if (!gFatRam) {
        gFatRam = (unsigned char *)NewPtr((long)FATRAM_SECS * 512);
        if (gFatRam) fat_ram_format();
    }
    if (gFatRam && fat12_mount_dev(fat_dev_ram)) return true;
#endif
    return false;
}

/* ---- FAT entries -------------------------------------------------------- */
static unsigned short fat_get(unsigned short cl)
{
    long off = (long)cl + ((long)cl >> 1);          /* cl * 1.5 */
    unsigned short v = (unsigned short)(gFatCache[off] | (gFatCache[off + 1] << 8));
    return (cl & 1) ? (unsigned short)(v >> 4) : (unsigned short)(v & 0x0FFF);
}

static void fat_set(unsigned short cl, unsigned short val)
{
    long off = (long)cl + ((long)cl >> 1);
    if (cl & 1) {
        gFatCache[off]     = (unsigned char)((gFatCache[off] & 0x0F) | ((val << 4) & 0xF0));
        gFatCache[off + 1] = (unsigned char)(val >> 4);
    } else {
        gFatCache[off]     = (unsigned char)(val & 0xFF);
        gFatCache[off + 1] = (unsigned char)((gFatCache[off + 1] & 0xF0) | ((val >> 8) & 0x0F));
    }
    gFatDirty = true;
}

static unsigned short fat_alloc(void)
{
    unsigned short cl;
    unsigned short max = (unsigned short)
        (2 + (gFatTotSec - gFatDataStart) / gFatSecPerClus);
    for (cl = 2; cl < max; cl++)
        if (fat_get(cl) == 0) return cl;
    return 0;
}

static Boolean fat_flush(void)
{
    short s, f;
    if (!gFatDirty) return true;
    for (f = 0; f < gFatNFats; f++)
        for (s = 0; s < gFatFatSz; s++)
            if (!gFatDev(true, (short)(gFatRsvd + f * gFatFatSz + s),
                         gFatCache + (long)s * 512))
                return false;
    gFatDirty = false;
    return true;
}

static short fat_cluster_lba(unsigned short cl)
{
    return (short)(gFatDataStart + (long)(cl - 2) * gFatSecPerClus);
}

/* ---- names -------------------------------------------------------------- */
static void fat_name_to_83(const char *in, unsigned char *out11)
{
    short i = 0, o = 0;
    memset(out11, ' ', 11);
    while (in[i] && in[i] != '.' && o < 8) {
        char c = in[i++];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out11[o++] = (unsigned char)c;
    }
    while (in[i] && in[i] != '.') i++;
    if (in[i] == '.') i++;
    o = 8;
    while (in[i] && o < 11) {
        char c = in[i++];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out11[o++] = (unsigned char)c;
    }
}

static void fat_83_to_name(const unsigned char *e, char *out)
{
    short i, o = 0;
    for (i = 0; i < 8 && e[i] != ' '; i++) out[o++] = (char)e[i];
    if (e[8] != ' ') {
        out[o++] = '.';
        for (i = 8; i < 11 && e[i] != ' '; i++) out[o++] = (char)e[i];
    }
    out[o] = 0;
}

/* ---- root directory ----------------------------------------------------- */
/* fat_dir_scan - iterate root entries; returns the (sector, offset) of a
   match by 8.3 name, of the first free slot, or fills the listing. */
static Boolean fat_find(const unsigned char *name11, short *sec, short *off)
{
    short s, o;
    for (s = 0; s < (gFatRootEnts * 32 + 511) / 512; s++) {
        if (!gFatDev(false, (short)(gFatRootStart + s), gFatSec)) return false;
        for (o = 0; o < 512; o += 32) {
            if (gFatSec[o] == 0x00) return false;       /* end of dir */
            if (gFatSec[o] == 0xE5) continue;           /* deleted */
            if (gFatSec[o + 11] & 0x08) continue;       /* volume label */
            if (memcmp(gFatSec + o, name11, 11) == 0) {
                *sec = s; *off = o; return true;
            }
        }
    }
    return false;
}

static Boolean fat_free_slot(short *sec, short *off)
{
    short s, o;
    for (s = 0; s < (gFatRootEnts * 32 + 511) / 512; s++) {
        if (!gFatDev(false, (short)(gFatRootStart + s), gFatSec)) return false;
        for (o = 0; o < 512; o += 32) {
            if (gFatSec[o] == 0x00 || gFatSec[o] == 0xE5) {
                *sec = s; *off = o; return true;
            }
        }
    }
    return false;
}

static void fat12_list(void)
{
    short s, o;
    gFatCount = 0;
    if (!gFatMounted) return;
    for (s = 0; s < (gFatRootEnts * 32 + 511) / 512; s++) {
        if (!gFatDev(false, (short)(gFatRootStart + s), gFatSec)) return;
        for (o = 0; o < 512; o += 32) {
            if (gFatSec[o] == 0x00) return;
            if (gFatSec[o] == 0xE5) continue;
            if (gFatSec[o + 11] & 0x18) continue;       /* label/dir */
            if (gFatCount >= FAT_LIST_MAX) return;
            fat_83_to_name(gFatSec + o, (char *)gFatNames[gFatCount]);
            gFatSizes[gFatCount] = (long)fat_rd32(gFatSec + o + 28);
            gFatCount++;
        }
    }
}

/* fat12_read - file by display name -> buf, returns bytes (0 on miss) */
static long fat12_read(const char *name, unsigned char *buf, long max)
{
    unsigned char n11[11];
    short sec, off;
    unsigned short cl;
    long size, got = 0;
    if (!gFatMounted) return 0;
    fat_name_to_83(name, n11);
    if (!fat_find(n11, &sec, &off)) return 0;
    size = (long)fat_rd32(gFatSec + off + 28);
    cl = fat_rd16(gFatSec + off + 26);
    if (size > max) size = max;
    while (got < size && cl >= 2 && cl < 0xFF8) {
        short s2;
        for (s2 = 0; s2 < gFatSecPerClus && got < size; s2++) {
            long take = size - got > 512 ? 512 : size - got;
            if (!gFatDev(false, (short)(fat_cluster_lba(cl) + s2), gFatSec))
                return got;
            memcpy(buf + got, gFatSec, take);
            got += take;
        }
        cl = fat_get(cl);
    }
    return got;
}

/* fat_free_chain - release a cluster chain */
static void fat_free_chain(unsigned short cl)
{
    while (cl >= 2 && cl < 0xFF8) {
        unsigned short nx = fat_get(cl);
        fat_set(cl, 0);
        cl = nx;
    }
}

/* fat12_write - create/overwrite a root file. Returns success. */
static Boolean fat12_write(const char *name, const unsigned char *buf, long len)
{
    unsigned char n11[11];
    short sec, off;
    unsigned short first = 0, prev = 0;
    long put = 0;
    if (!gFatMounted) return false;
    fat_name_to_83(name, n11);
    /* overwrite: free the old chain, reuse the entry */
    if (fat_find(n11, &sec, &off)) {
        fat_free_chain(fat_rd16(gFatSec + off + 26));
    } else {
        if (!fat_free_slot(&sec, &off)) return false;
        memset(gFatSec + off, 0, 32);
        memcpy(gFatSec + off, n11, 11);
        gFatSec[off + 11] = 0x20;                       /* archive */
    }
    /* allocate + write the data */
    while (put < len) {
        unsigned short cl = fat_alloc();
        short s2;
        if (!cl) { fat_free_chain(first); return false; }
        fat_set(cl, 0xFFF);                             /* tentative EOC */
        if (prev) fat_set(prev, cl);
        else first = cl;
        prev = cl;
        for (s2 = 0; s2 < gFatSecPerClus && put < len; s2++) {
            unsigned char data[512];
            long take = len - put > 512 ? 512 : len - put;
            memset(data, 0, 512);
            memcpy(data, buf + put, take);
            if (!gFatDev(true, (short)(fat_cluster_lba(cl) + s2), data))
                return false;
            put += take;
        }
    }
    /* refresh the entry (fat_find/fat_free_slot left its sector in gFatSec,
       but the data writes reused the buffer - re-read, patch, write) */
    if (!gFatDev(false, (short)(gFatRootStart + sec), gFatSec)) return false;
    if (gFatSec[off] == 0x00 || gFatSec[off] == 0xE5 ||
        memcmp(gFatSec + off, n11, 11) != 0) {
        memset(gFatSec + off, 0, 32);
        memcpy(gFatSec + off, n11, 11);
        gFatSec[off + 11] = 0x20;
    }
    fat_wr16(gFatSec + off + 26, first);
    fat_wr32(gFatSec + off + 28, (unsigned long)len);
    if (!gFatDev(true, (short)(gFatRootStart + sec), gFatSec)) return false;
    return fat_flush();
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
static short   gFVol = 0;                   /* 0 = HFS, 1 = PC floppy (FAT12) */
static Boolean gNFat = false;               /* Notepad buffer came from FAT */
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
    short count = gFVol ? gFatCount : gFCount;

    text_at(x, y0 + 10, "Name", C_CYAN, C_BLUE, false);
    text_at(r.right - 80, y0 + 10, "Size", C_CYAN, C_BLUE, false);
    text_at(r.right - 150, y0 + 10, gFVol ? "(PC disk)" : "(HFS)", C_MAG, C_BLUE, false);

    if (gFVol && count == 0)
        text_at(x, y0 + 26, "no files on the PC volume", C_WHITE, C_BLUE, false);

    for (i = 0; i < FROWS; i++) {
        short fi = gFTop + i;
        short ry = y0 + 14 + i * FROW_H;
        Boolean sel;
        SetRect(&row, r.left + 2, ry, r.right - 2, ry + FROW_H);
        if (fi >= count) break;
        sel = (fi == gFSel);
#if UNO_COLOR
        if (sel) uno_fill(&row, C_CYAN);    /* explicit palette selection bar
                                               (InvertRect is index-inversion
                                               in 8-bit - off-palette) */
#endif
        if (gFVol) {
            strcpy(line, (const char *)gFatNames[fi]);
            text_at_max(x, ry + 12, line, sel ? C_BLUE : C_WHITE, r.right - r.left - 100);
            fmt_u(gFatSizes[fi], num);
            text_at(r.right - 80, ry + 12, num, sel ? C_BLUE : C_WHITE, C_BLUE, false);
        } else {
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
        }
#if !UNO_COLOR
        if (sel) uno_invert(&row);          /* 1-bit invert is the classic look */
#endif
    }
    text_at(x, r.bottom - 6, "Enter: open   R: refresh   V: volume", C_CYAN, C_BLUE, false);
}

static void notepad_load_pascal(const unsigned char *pname);

static void notepad_load_fat(const char *name);

static void files_open_sel(void)
{
    if (gFVol) {
        if (gFatCount == 0 || gFSel >= gFatCount) return;
        notepad_load_fat((const char *)gFatNames[gFSel]);
        launch_app(APP_NOTEPAD);
        return;
    }
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
    short count = gFVol ? gFatCount : gFCount;
    if (ch == 'v' || ch == 'V') {                       /* cycle volume */
        if (!gFVol) {
            if (fat12_mount()) { gFVol = 1; fat12_list(); gFSel = 0; gFTop = 0; }
        } else {
            gFVol = 0; gFSel = 0; gFTop = 0;
        }
        if (w) draw_window(w);
        return true;
    }
    if (code == 0x7D || ch == 0x1F) {                   /* down */
        if (gFSel < count - 1) gFSel++;
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
        if (gFVol) fat12_list(); else files_refresh();
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
    short count = gFVol ? gFatCount : gFCount;
    if (row < 0 || gFTop + row >= count) return;
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
static unsigned char gNFile[32] = "\014UNTITLED.TXT";

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
    gNLen = 0; gNCaret = 0; gNTop = 0; gNDirty = false; gNFat = false;
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

/* notepad_load_fat - open a file from the PC floppy volume */
static void notepad_load_fat(const char *name)
{
    long got;
    short n = (short)strlen(name); if (n > 31) n = 31;
    gNFile[0] = (unsigned char)n; memcpy(gNFile + 1, name, n);
    gNLen = 0; gNCaret = 0; gNTop = 0; gNDirty = false; gNFat = true;
    got = fat12_read(name, (unsigned char *)gNBuf, NBUF - 1);
    gNLen = (short)got;
    { short i; for (i = 0; i < gNLen; i++) if (gNBuf[i] == '\n') gNBuf[i] = '\r'; }
}

static void notepad_save(void)
{
    short ref;
    long count = gNLen;
    OSErr err;
    if (gNFat) {                    /* the buffer came from the PC disk */
        char name[32]; short n = gNFile[0];
        memcpy(name, gNFile + 1, n); name[n] = 0;
        if (fat12_write(name, (unsigned char *)gNBuf, gNLen)) {
            gNDirty = false;
            fat12_list();
        }
        return;
    }
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
 * Tracker (Amiga-parity) - the 32-row x 4-channel pattern editor from
 * amiga/tracker.i on the Sound Manager. Channels 1-3 are square-wave
 * synth channels; channel 4 ("Nz") plays a low two-octave-down thump in
 * place of noise (the square synth has no noise source). Pattern format
 * is byte-identical to the Amiga/Genesis trackers: 32 rows x 4 channels
 * x (note 1..24 = C-2..B-3, instrument 0-3). s/l persist SONG.TRK
 * through the File Manager. If the Sound Manager is unavailable the
 * editor runs visual-only (same rule as Music).
 * ========================================================================= */
#define TK_ROWS  32
#define TK_CHANS 4
#define TK_VIEW  14
#define TK_PATLEN (TK_ROWS * TK_CHANS * 2)

static unsigned char gTkPat[TK_PATLEN];
static short   gTkRow = 0, gTkCh = 0, gTkTop = 0, gTkPRow = 0;
static Boolean gTkPlaying = false;
static long    gTkLast = 0;
static SndChannelPtr gTkSnd[TK_CHANS];

static const char kTkNoteNames[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
static const char kTkInstName[]  = "SSTN";   /* SQ SW TR NZ initials */

/* demo song - byte-identical to the Amiga tracker's */
static const unsigned char kTkDemo[TK_PATLEN] = {
    1,1, 13,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 17,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0,
    1,1, 20,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 17,0, 13,2,  0,0,  0,0, 0,0, 0,0, 0,0,
    8,1, 13,0, 17,2, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 15,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0,
    8,1, 20,0, 15,2, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 15,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0,
    6,1, 10,0, 13,2, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 13,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0,
    6,1, 17,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 13,0, 10,2,  0,0,  0,0, 0,0, 0,0, 0,0,
    8,1, 11,0, 15,2, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 15,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0,
    8,1, 20,0, 19,2, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 23,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0
};

static unsigned char *tk_cell(short row, short ch)
{
    return &gTkPat[(row * TK_CHANS + ch) * 2];
}

static void tk_open_chans(void)
{
    short i;
    for (i = 0; i < TK_CHANS; i++)
        if (!gTkSnd[i] && SndNewChannel(&gTkSnd[i], noteSynth, 0, NULL) != noErr)
            gTkSnd[i] = NULL;
}

static void tk_quiet(void)
{
    short i; SndCommand c;
    for (i = 0; i < TK_CHANS; i++) {
        if (!gTkSnd[i]) continue;
        c.cmd = quietCmd; c.param1 = 0; c.param2 = 0;
        SndDoImmediate(gTkSnd[i], &c);
    }
}

static void tk_close_chans(void)
{
    short i;
    for (i = 0; i < TK_CHANS; i++)
        if (gTkSnd[i]) { SndDisposeChannel(gTkSnd[i], true); gTkSnd[i] = NULL; }
}

static void tk_stop(void)
{
    gTkPlaying = false;
    tk_quiet();
}

/* tk_trigger_row - fire the row's notes. note 1 = C4 (the same pitch
 * the 68K ports play for "C-2"); the noise channel thumps low. */
static void tk_trigger_row(short row)
{
    short ch; SndCommand c;
    for (ch = 0; ch < TK_CHANS; ch++) {
        unsigned char *cell = tk_cell(row, ch);
        if (!cell[0] || !gTkSnd[ch]) continue;
        c.cmd = noteCmd;
        if (ch == 3) { c.param1 = 4 * 33;  c.param2 = (short)(36 + (cell[0] % 12)); }
        else         { c.param1 = 30 * 33; c.param2 = (short)(59 + cell[0]); }
        SndDoImmediate(gTkSnd[ch], &c);
    }
}

static void tk_fmt_cell(const unsigned char *cell, char *out)
{
    if (!cell[0]) { strcpy(out, "--- -"); return; }
    {
        short n = (short)(cell[0] - 1);
        out[0] = kTkNoteNames[(n % 12) * 2];
        out[1] = kTkNoteNames[(n % 12) * 2 + 1];
        out[2] = (char)('2' + n / 12);
        out[3] = ' ';
        out[4] = kTkInstName[cell[1] & 3];
        out[5] = 0;
    }
}

static void tracker_draw(UnoWin *w)
{
    Rect r = w->bounds, ct = r;
    short x0 = (short)(r.left + 10), y0 = (short)(r.top + TBAR_H + 14), y, i, ch;
    char buf[8];

    ct.top += TBAR_H; InsetRect(&ct, 1, 1); uno_fill(&ct, C_BLUE);

    /* header: Row + channel names, the cursor channel highlighted */
    text_at(x0, y0, "Row", C_CYAN, C_BLUE, false);
    {
        static const char *chn[TK_CHANS] = { "Ch1", "Ch2", "Ch3", "Nz" };
        for (ch = 0; ch < TK_CHANS; ch++)
            text_at((short)(x0 + 44 + ch * 64), y0, chn[ch],
                    (short)(ch == gTkCh ? C_MAG : C_CYAN), C_BLUE, false);
    }
    /* keep the cursor row in view */
    if (gTkRow < gTkTop) gTkTop = gTkRow;
    if (gTkRow >= gTkTop + TK_VIEW) gTkTop = (short)(gTkRow - TK_VIEW + 1);
    /* rows */
    for (i = 0; i < TK_VIEW; i++) {
        short row = (short)(gTkTop + i);
        y = (short)(y0 + 16 + i * NLINE_H);
        if (row == gTkRow) {                    /* cursor bar */
            Rect bar; SetRect(&bar, (short)(r.left + 4), (short)(y - 11),
                              (short)(r.right - 4), (short)(y + 3));
            uno_fill(&bar, C_CYAN);
        } else if (gTkPlaying && row == gTkPRow) {
            Rect bar; SetRect(&bar, (short)(r.left + 4), (short)(y - 11),
                              (short)(r.right - 4), (short)(y + 3));
            uno_fill(&bar, C_MAG);
        }
        put2(row, buf);
        text_at(x0, y, buf, (short)(row == gTkRow ? C_BLUE : C_WHITE), C_BLUE, false);
        for (ch = 0; ch < TK_CHANS; ch++) {
            tk_fmt_cell(tk_cell(row, ch), buf);
            text_at((short)(x0 + 44 + ch * 64), y, buf,
                    (short)(row == gTkRow ? C_BLUE : C_WHITE), C_BLUE, false);
        }
    }
    /* footers */
    text_at(x0, (short)(r.bottom - 22), "q/w:note e:inst x:clr d:demo s/l:save",
            C_CYAN, C_BLUE, false);
    text_at(x0, (short)(r.bottom - 8),
            gTkPlaying ? "Space: stop   arrows: move"
                       : "Space: play   arrows: move", C_CYAN, C_BLUE, false);
}

static void tk_redraw(void)
{
    UnoWin *w = find_app_window(APP_TRACKER);
    if (w) draw_window(w);
}

static void tk_save(void)
{
    short ref; long count = TK_PATLEN;
    OSErr err = Create("\010SONG.TRK", 0, 'UNOD', 'UTRK');
    if (err != noErr && err != dupFNErr) return;
    if (FSOpen("\010SONG.TRK", 0, &ref) != noErr) return;
    FSWrite(ref, &count, (Ptr)gTkPat);
    FSClose(ref);
    FlushVol(NULL, 0);
}

static void tk_load(void)
{
    short ref; long count = TK_PATLEN;
    if (FSOpen("\010SONG.TRK", 0, &ref) != noErr) return;
    FSRead(ref, &count, (Ptr)gTkPat);           /* partial reads are fine */
    FSClose(ref);
}

static void tracker_tick(void)
{
    UnoWin *w;
    if (!gTkPlaying) return;
    if (TickCount() - gTkLast < 6) return;      /* same tempo as the 68Ks */
    gTkLast = TickCount();
    gTkPRow++;
    if (gTkPRow >= TK_ROWS) gTkPRow = 0;
    tk_trigger_row(gTkPRow);
    w = find_app_window(APP_TRACKER);
    if (w && zwin(gZCount - 1) == w) draw_window(w);
}

static Boolean tracker_key(char ch, short code)
{
    unsigned char *cell = tk_cell(gTkRow, gTkCh);
    if (code == 0x7E || ch == 0x1E) {           /* up */
        if (gTkRow > 0) gTkRow--;
    } else if (code == 0x7D || ch == 0x1F) {    /* down */
        if (gTkRow < TK_ROWS - 1) gTkRow++;
    } else if (code == 0x7B || ch == 0x1C) {    /* left */
        if (gTkCh > 0) gTkCh--;
    } else if (code == 0x7C || ch == 0x1D) {    /* right */
        if (gTkCh < TK_CHANS - 1) gTkCh++;
    } else if (ch == 'q') {
        if (!cell[0]) cell[0] = 1;
        else if (cell[0] > 1) cell[0]--;
        tk_open_chans(); tk_trigger_row(gTkRow);
    } else if (ch == 'w') {
        if (!cell[0]) cell[0] = 1;
        else if (cell[0] < 24) cell[0]++;
        tk_open_chans(); tk_trigger_row(gTkRow);
    } else if (ch == 'e') {
        if (cell[0]) cell[1] = (unsigned char)((cell[1] + 1) & 3);
    } else if (ch == 'x') {
        cell[0] = 0; cell[1] = 0;
    } else if (ch == 'd') {
        memcpy(gTkPat, kTkDemo, TK_PATLEN);
    } else if (ch == 's') {
        tk_save();
    } else if (ch == 'l') {
        tk_load();
    } else if (ch == ' ') {
        if (gTkPlaying) tk_stop();
        else {
            music_stop(); gm_stop();            /* the Tracker owns audio */
            tk_open_chans();
            gTkPlaying = true;
            gTkPRow = TK_ROWS - 1;              /* first tick wraps to 0 */
            gTkLast = TickCount() - 6;
        }
    } else {
        return false;
    }
    tk_redraw();
    return true;
}

/* =========================================================================
 * Paint - MacPaint-style editor (implementation below the dispatch).
 * ========================================================================= */
static void paint_draw(UnoWin *w);
static Boolean paint_key(char ch, short code);
static void paint_click(UnoWin *w, Point p);
static void paint_open(void);

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
 * Cooperative scheduler (milestone 3) - the Mac edition of
 * amiga/scheduler.i / genesis/scheduler.i. Task 0 is the kernel task
 * (the Toolbox event loop, drag, audio services, desktop); every open
 * window runs its app proc in its own task with a private heap stack.
 * Context switches happen only at task_yield / the task body's mailbox
 * wait - cooperative, so QuickDraw access never interleaves.
 *
 * Keys for the focused window and per-frame ticks are posted into the
 * task's one-slot mailbox by the kernel task. Key posts use a bounded
 * yield-retry so typing bursts survive the single slot.
 *
 * The 68000 context switch lives in ctx_switch (asm below): callee-saved
 * registers (d2-d7/a2-a6, A5 world included) are stacked, SP is swapped.
 * The classic Mac OS "stack sniffer" VBL task would flag a stack outside
 * the application stack zone as sysError 28, so sched_init clears
 * StkLowPt ($110) - the documented opt-out the Thread Manager itself
 * uses.
 * ========================================================================= */
#define NTASKS  (MAXWIN + 1)
#define TSTK_SZ 8192L

typedef struct {
    long  *sp;                      /* saved stack pointer */
    char   state;                   /* 0 = free, 1 = ready */
    char   evt;                     /* mailbox: 0 none, 1 key, 2 tick */
    char   cmd;                     /* key event: Cmd modifier */
    char   pad;
    short  d1, d2;                  /* key ascii, key code */
} UnoTask;

static UnoTask gTasks[NTASKS];
static short   gCurTask = 0;
static char   *gTaskStk[MAXWIN];    /* heap stacks (kept out of the A5 world) */

/* The real coroutine scheduler is 68K-only (ctx_switch swaps the SP after
 * stacking the callee-saved registers). On the PS2 (MIPS R5900) and the host
 * shim, we ship the same "poll-and-dispatch" / kernel-driven scheduler the
 * Apple II port settled on: no per-app stack, keys and frame ticks are
 * dispatched straight to the app handlers by the kernel task. Semantics are
 * identical for every app (none rely on preemption); only the plumbing differs.
 * Revisit real EE threads only if some app ever needs to block (HANDOFF SS7). */
#ifdef __m68k__
#define UNO_COROUTINE_SCHED 1
/* ctx_switch(&old->sp, new_sp): stack the callee-saved registers, swap
 * stacks, unstack, return on the other side. Args at 48/52(sp) after
 * the 44-byte movem frame + return address. */
void ctx_switch(long **save_sp, long *new_sp);
asm(
    ".text\n"
    ".globl ctx_switch\n"
    "ctx_switch:\n"
    "    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
    "    move.l  48(%sp),%a0\n"
    "    move.l  %sp,(%a0)\n"
    "    move.l  52(%sp),%sp\n"
    "    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
    "    rts\n"
);
#endif

static void app_tick_dispatch(short proc)
{
    switch (proc) {
    case APP_DOSTRIS: dostris_tick(); break;
    case APP_OUTLAST: outlast_tick(); break;
    case APP_PACMAN:  pacman_tick();  break;
    }
}

#ifdef UNO_COROUTINE_SCHED
/* task_body - generic app task: wait on the mailbox (yielding), then
 * dispatch to the proc handlers. Window/proc are re-derived per event. */
static void task_body(void)
{
    for (;;) {
        UnoTask *t = &gTasks[gCurTask];
        char type; short d1, d2; Boolean cmd;
        while (!t->evt) task_yield();
        type = t->evt; d1 = t->d1; d2 = t->d2; cmd = (t->cmd != 0);
        t->evt = 0;
        {
            short slot = gCurTask - 1;
            if (slot >= 0 && slot < MAXWIN && gWins[slot].used) {
                if (type == 1) app_key(gWins[slot].proc, (char)d1, d2, cmd);
                else           app_tick_dispatch(gWins[slot].proc);
            }
        }
    }
}

static void sched_init(void)
{
    short i;
    for (i = 0; i < NTASKS; i++) {
        gTasks[i].state = 0; gTasks[i].evt = 0; gTasks[i].sp = NULL;
    }
    gTasks[0].state = 1;            /* kernel task always ready */
    gCurTask = 0;
    for (i = 0; i < MAXWIN; i++)
        if (!gTaskStk[i]) gTaskStk[i] = NewPtr(TSTK_SZ);
    *(long *)0x110 = 0;             /* StkLowPt: disable the stack sniffer */
}

static void task_spawn(short slot)
{
    UnoTask *t = &gTasks[slot + 1];
    long *sp;
    short i;
    if (!gTaskStk[slot]) return;    /* no stack: app runs kernel-driven */
    sp = (long *)(gTaskStk[slot] + TSTK_SZ);
    *--sp = (long)task_body;        /* rts target after the register pop */
    for (i = 0; i < 11; i++) *--sp = 0;  /* d2-d7/a2-a6 */
    t->sp = sp;
    t->state = 1;
    t->evt = 0;
}

static void task_kill(short slot)
{
    gTasks[slot + 1].state = 0;
    gTasks[slot + 1].evt = 0;
}

static void task_yield(void)
{
    short next = gCurTask, prev;
    do { next = (short)((next + 1) % NTASKS); } while (!gTasks[next].state);
    if (next == gCurTask) return;
    prev = gCurTask;
    gCurTask = next;
    ctx_switch(&gTasks[prev].sp, gTasks[next].sp);
}

static void task_post(short slot, char type, short d1, short d2, Boolean cmd)
{
    UnoTask *t = &gTasks[slot + 1];
    if (!t->state || t->evt) return;    /* no task / mailbox full: drop */
    t->d1 = d1; t->d2 = d2; t->cmd = (char)(cmd ? 1 : 0);
    t->evt = type;
}

static void task_post_key(short slot, short d1, short d2, Boolean cmd)
{
    UnoTask *t = &gTasks[slot + 1];
    short spins = 100;              /* bounded: a wedged task drops keys */
    if (!t->state) return;
    while (t->evt && spins--) task_yield();
    if (t->evt) return;
    t->d1 = d1; t->d2 = d2; t->cmd = (char)(cmd ? 1 : 0);
    t->evt = 1;
}

/* post_ticks - a frame tick for the topmost window's task */
static void post_ticks(void)
{
    if (gZCount > 0)
        task_post(gZ[gZCount - 1], 2, 0, 0, false);
}

#else  /* ---- kernel-driven (poll-and-dispatch) scheduler: PS2 / host ---- */

/* No coroutines: keys and frame ticks go straight to the app handlers and the
 * focused window is repainted. Identical app behaviour, no context switch. */
static void sched_init(void)  { gTasks[0].state = 1; gCurTask = 0; }
static void task_spawn(short slot) { (void)slot; }   /* apps are kernel-driven */
static void task_kill(short slot)  { (void)slot; }
static void task_yield(void)  {}

static void task_post(short slot, char type, short d1, short d2, Boolean cmd)
{
    if (slot < 0 || slot >= MAXWIN || !gWins[slot].used) return;
    if (type == 1) app_key(gWins[slot].proc, (char)d1, d2, cmd);
    else           app_tick_dispatch(gWins[slot].proc);
    draw_window(&gWins[slot]);
}
static void task_post_key(short slot, short d1, short d2, Boolean cmd)
{ task_post(slot, 1, d1, d2, cmd); }

static void post_ticks(void)
{
    if (gZCount > 0) task_post(gZ[gZCount - 1], 2, 0, 0, false);
}

#endif /* UNO_COROUTINE_SCHED */

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
    case APP_DOSTRIS: dostris_draw(w); break;
    case APP_OUTLAST: outlast_draw(w); break;
    case APP_PACMAN:  pacman_draw(w);  break;
    case APP_TRACKER: tracker_draw(w); break;
    case APP_PAINT:   paint_draw(w);   break;
#if UNO_COLOR
    case APP_THEME:   theme_draw(w);   break;
#endif
    }
}

static Boolean app_key(short proc, char ch, short code, Boolean cmd)
{
    switch (proc) {
    case APP_FILES:   if (!cmd) return files_key(ch, code); break;
    case APP_NOTEPAD: return notepad_key(ch, code, cmd);
    case APP_MUSIC:   if (!cmd) return music_key(ch, code); break;
    case APP_DOSTRIS: if (!cmd) return dostris_key(ch, code); break;
    case APP_OUTLAST: if (!cmd) return outlast_key(ch, code); break;
    case APP_PACMAN:  if (!cmd) return pacman_key(ch, code); break;
    case APP_TRACKER: if (!cmd) return tracker_key(ch, code); break;
    case APP_PAINT:   if (!cmd) return paint_key(ch, code); break;
#if UNO_COLOR
    case APP_THEME:   if (!cmd) return theme_key(ch, code); break;
#endif
    }
    return false;
}

static void app_click(short proc, UnoWin *w, Point p)
{
    if (proc == APP_FILES) files_click(w, p);
    if (proc == APP_PAINT) paint_click(w, p);
}

static void app_opened(short proc)
{
    if (proc == APP_FILES) files_refresh();
    if (proc == APP_MUSIC) music_open_chan();
    if (proc == APP_PAINT) paint_open();
}

static void app_close(short proc)
{
    if (proc == APP_MUSIC) {
        music_stop();
        if (gSnd) { SndDisposeChannel(gSnd, true); gSnd = NULL; }
    }
    if (proc == APP_TRACKER) {
        tk_stop();
        tk_close_chans();
    }
}

/* =========================================================================
 * Paint - MacPaint-style bitmap editor (the shared UnoDOS Paint design:
 * tool palette down the left, color/pattern strip along the bottom,
 * drag-to-draw canvas). Tools: pencil, brush, eraser, line, frame rect,
 * filled rect, frame oval, filled oval, flood fill, spray.
 *
 * Color selector ("all the platform's colors"):
 *   UnoDOS7      - the full 8-bit indexed gamut a CLUT Mac can show at
 *                  once: a 6x6x6 RGB cube + 16 grays + 24 hue ramps =
 *                  256 colors, picked from a full-screen 16x16 grid
 *                  ('c' or the current-color swatch opens it).
 *   UnoDOSClassic- authentic 1-bit MacPaint: black/white plus the
 *                  classic dither patterns in the bottom strip; painting
 *                  applies the pattern bit per pixel.
 *
 * The canvas backing store is a byte-per-pixel heap block (color: a
 * palette index; mono: 0/1), repainted as horizontal runs - strokes
 * draw incrementally, full repaints only on window redraws. The drag
 * loop is synchronous (StillDown/GetMouse), the classic Mac idiom.
 * ========================================================================= */
#define PT_W      408
#define PT_H      240
#define PT_TOOLS  10
#define PT_TOOLW  26
#define PT_CELL   24

enum { T_PENCIL = 0, T_BRUSH, T_ERASER, T_LINE, T_RECT,
       T_FRECT, T_OVAL, T_FOVAL, T_FILL, T_SPRAY };

static unsigned char *gPtCanvas = NULL;     /* PT_W * PT_H bytes */
static short   gPtTool = T_PENCIL;
static Boolean gPtPicker = false;           /* color picker overlay up */

#if UNO_COLOR
static short    gPtColor = 0;               /* index into kPtPal */
static RGBColor kPtPal[256];
static Boolean  kPtPalInit = false;
#define PT_BG   255                         /* white (built last, below) */

static void pt_build_palette(void)
{
    short i, r, g, b;
    if (kPtPalInit) return;
    /* 0..215: 6x6x6 RGB cube */
    i = 0;
    for (r = 0; r < 6; r++) for (g = 0; g < 6; g++) for (b = 0; b < 6; b++) {
        kPtPal[i].red   = (unsigned short)(r * 13107);
        kPtPal[i].green = (unsigned short)(g * 13107);
        kPtPal[i].blue  = (unsigned short)(b * 13107);
        i++;
    }
    /* 216..231: 16 grays */
    for (g = 0; g < 16; g++) {
        kPtPal[i].red = kPtPal[i].green = kPtPal[i].blue =
            (unsigned short)(g * 4369);
        i++;
    }
    /* 232..254: hue ramps (pure + half-bright primaries/secondaries) */
    {
        static const unsigned char hues[23][3] = {
            {5,0,0},{5,2,0},{5,4,0},{4,5,0},{2,5,0},{0,5,0},{0,5,2},{0,5,4},
            {0,4,5},{0,2,5},{0,0,5},{2,0,5},{4,0,5},{5,0,4},{5,0,2},
            {3,1,0},{3,3,1},{1,3,1},{1,3,3},{1,1,3},{3,1,3},{3,2,1},{2,1,0}
        };
        for (g = 0; g < 23; g++) {
            kPtPal[i].red   = (unsigned short)(hues[g][0] * 13107);
            kPtPal[i].green = (unsigned short)(hues[g][1] * 13107);
            kPtPal[i].blue  = (unsigned short)(hues[g][2] * 13107);
            i++;
        }
    }
    /* 255: white (the canvas background) */
    kPtPal[255].red = kPtPal[255].green = kPtPal[255].blue = 0xFFFF;
    kPtPalInit = true;
}
#else
static short gPtPat = 1;                    /* pattern index (1 = black) */
#define PT_BG   0
/* the classic dither set: white, black, 25%, 50%, 75%, vert, horz,
   checker-2, diagonal, brick */
static const unsigned char kPtPats[10][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},   /* white  */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},   /* black  */
    {0x88,0x00,0x22,0x00,0x88,0x00,0x22,0x00},   /* 25%    */
    {0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55},   /* 50%    */
    {0x77,0xFF,0xDD,0xFF,0x77,0xFF,0xDD,0xFF},   /* 75%    */
    {0x88,0x88,0x88,0x88,0x88,0x88,0x88,0x88},   /* vert   */
    {0x00,0x00,0xFF,0x00,0x00,0x00,0xFF,0x00},   /* horz   */
    {0xCC,0xCC,0x33,0x33,0xCC,0xCC,0x33,0x33},   /* check2 */
    {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01},   /* diag   */
    {0xFF,0x80,0x80,0x80,0xFF,0x08,0x08,0x08},   /* brick  */
};
#endif

/* canvas geometry inside the window */
static void pt_canvas_rect(UnoWin *w, Rect *r)
{
    r->left   = (short)(w->bounds.left + PT_TOOLW + 34);
    r->top    = (short)(w->bounds.top + TBAR_H + 4);
    r->right  = (short)(r->left + PT_W);
    r->bottom = (short)(r->top + PT_H);
}

static unsigned char pt_ink(short x, short y)
{
#if UNO_COLOR
    (void)x; (void)y;
    return (unsigned char)gPtColor;
#else
    /* the current pattern decides each pixel (authentic dithering) */
    return (unsigned char)((kPtPats[gPtPat][y & 7] >> (7 - (x & 7))) & 1);
#endif
}

static void pt_show_px(UnoWin *w, short x, short y)
{
    Rect cr, px;
    if (!gPtCanvas) return;
    pt_canvas_rect(w, &cr);
    SetRect(&px, (short)(cr.left + x), (short)(cr.top + y),
                 (short)(cr.left + x + 1), (short)(cr.top + y + 1));
#if UNO_COLOR
    RGBForeColor(&kPtPal[gPtCanvas[(long)y * PT_W + x]]);
    PaintRect(&px);
    RGBForeColor(&kBlack);
#else
    if (gPtCanvas[(long)y * PT_W + x]) PaintRect(&px);
    else { PenMode(patBic); PaintRect(&px); PenNormal(); }
#endif
}

static void pt_set_px(UnoWin *w, short x, short y, unsigned char v)
{
    if (x < 0 || y < 0 || x >= PT_W || y >= PT_H || !gPtCanvas) return;
    gPtCanvas[(long)y * PT_W + x] = v;
    pt_show_px(w, x, y);
}

static void pt_dot(UnoWin *w, short x, short y, short size, Boolean erase)
{
    short dx, dy;
    for (dy = 0; dy < size; dy++)
        for (dx = 0; dx < size; dx++) {
            short px = (short)(x + dx - size / 2), py = (short)(y + dy - size / 2);
            pt_set_px(w, px, py, erase ? PT_BG : pt_ink(px, py));
        }
}

static void pt_line(UnoWin *w, short x0, short y0, short x1, short y1, short size)
{
    /* Bresenham */
    short dx = (short)(x1 > x0 ? x1 - x0 : x0 - x1);
    short dy = (short)(y1 > y0 ? y1 - y0 : y0 - y1);
    short sx = (short)(x0 < x1 ? 1 : -1), sy = (short)(y0 < y1 ? 1 : -1);
    short err = (short)(dx - dy);
    for (;;) {
        pt_dot(w, x0, y0, size, false);
        if (x0 == x1 && y0 == y1) break;
        { short e2 = (short)(err * 2);
          if (e2 > -dy) { err -= dy; x0 += sx; }
          if (e2 <  dx) { err += dx; y0 += sy; } }
    }
}

static void pt_rect_shape(UnoWin *w, short x0, short y0, short x1, short y1, Boolean filled)
{
    short t, x, y;
    if (x1 < x0) { t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { t = y0; y0 = y1; y1 = t; }
    if (filled) {
        for (y = y0; y <= y1; y++)
            for (x = x0; x <= x1; x++) pt_set_px(w, x, y, pt_ink(x, y));
    } else {
        for (x = x0; x <= x1; x++) { pt_set_px(w, x, y0, pt_ink(x, y0)); pt_set_px(w, x, y1, pt_ink(x, y1)); }
        for (y = y0; y <= y1; y++) { pt_set_px(w, x0, y, pt_ink(x0, y)); pt_set_px(w, x1, y, pt_ink(x1, y)); }
    }
}

static void pt_oval_shape(UnoWin *w, short x0, short y0, short x1, short y1, Boolean filled)
{
    /* midpoint-ish: scan rows of the bounding box, solve the ellipse */
    short t, y;
    long a, b, cx2, cy2;
    if (x1 < x0) { t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { t = y0; y0 = y1; y1 = t; }
    a = (x1 - x0) / 2; b = (y1 - y0) / 2;
    if (a == 0 || b == 0) { pt_rect_shape(w, x0, y0, x1, y1, filled); return; }
    cx2 = x0 + a; cy2 = y0 + b;
    for (y = y0; y <= y1; y++) {
        long dy = y - cy2;
        long r2 = (a * a) - (a * a * dy * dy) / (b * b);
        long half = 0, x;
        while ((half + 1) * (half + 1) <= r2) half++;
        if (filled) {
            for (x = cx2 - half; x <= cx2 + half; x++)
                pt_set_px(w, (short)x, y, pt_ink((short)x, y));
        } else {
            pt_set_px(w, (short)(cx2 - half), y, pt_ink((short)(cx2 - half), y));
            pt_set_px(w, (short)(cx2 + half), y, pt_ink((short)(cx2 + half), y));
        }
    }
}

#define PT_STK 1024
static void pt_flood(UnoWin *w, short x, short y)
{
    unsigned char from, to;
    short *stk; long n = 0;
    if (x < 0 || y < 0 || x >= PT_W || y >= PT_H || !gPtCanvas) return;
    from = gPtCanvas[(long)y * PT_W + x];
    to = pt_ink(x, y);
#if UNO_COLOR
    if (from == to) return;
#else
    if (from == (pt_ink(0,0) ? 1 : 0) && from == to) return;
#endif
    stk = (short *)NewPtr(PT_STK * 4L);
    if (!stk) return;
    stk[n * 2] = x; stk[n * 2 + 1] = y; n++;
    while (n > 0) {
        short px, py, lx, rx, i;
        n--; px = stk[n * 2]; py = stk[n * 2 + 1];
        if (gPtCanvas[(long)py * PT_W + px] != from) continue;
        lx = px; while (lx > 0 && gPtCanvas[(long)py * PT_W + lx - 1] == from) lx--;
        rx = px; while (rx < PT_W - 1 && gPtCanvas[(long)py * PT_W + rx + 1] == from) rx++;
        for (i = lx; i <= rx; i++)
            pt_set_px(w, i, py, pt_ink(i, py));
        for (i = lx; i <= rx; i++) {
            if (py > 0 && gPtCanvas[(long)(py - 1) * PT_W + i] == from && n < PT_STK) {
                stk[n * 2] = i; stk[n * 2 + 1] = (short)(py - 1); n++;
            }
            if (py < PT_H - 1 && gPtCanvas[(long)(py + 1) * PT_W + i] == from && n < PT_STK) {
                stk[n * 2] = i; stk[n * 2 + 1] = (short)(py + 1); n++;
            }
        }
    }
    DisposePtr((Ptr)stk);
}

/* ---- chrome ------------------------------------------------------------ */
static void pt_tool_rect(UnoWin *w, short i, Rect *r)
{
    short col = (short)(i % 2), row = (short)(i / 2);
    r->left = (short)(w->bounds.left + 6 + col * (PT_CELL + 2));
    r->top  = (short)(w->bounds.top + TBAR_H + 6 + row * (PT_CELL + 2));
    r->right  = (short)(r->left + PT_CELL);
    r->bottom = (short)(r->top + PT_CELL);
}

static void pt_draw_toolglyph(short i, Rect *r)
{
    Rect g;
    short cx = (short)((r->left + r->right) / 2);
    short cy = (short)((r->top + r->bottom) / 2);
    switch (i) {
    case T_PENCIL:
        MoveTo((short)(r->left + 5), (short)(r->bottom - 5));
        LineTo((short)(r->right - 5), (short)(r->top + 5));
        break;
    case T_BRUSH:
        SetRect(&g, (short)(cx - 3), (short)(cy - 5), (short)(cx + 3), (short)(cy + 2));
        PaintOval(&g);
        MoveTo(cx, (short)(cy + 2)); LineTo(cx, (short)(cy + 6));
        break;
    case T_ERASER:
        SetRect(&g, (short)(cx - 6), (short)(cy - 4), (short)(cx + 6), (short)(cy + 4));
        FrameRect(&g);
        break;
    case T_LINE:
        MoveTo((short)(r->left + 4), (short)(r->bottom - 6));
        LineTo((short)(r->right - 4), (short)(r->top + 6));
        break;
    case T_RECT:
        SetRect(&g, (short)(cx - 7), (short)(cy - 5), (short)(cx + 7), (short)(cy + 5));
        FrameRect(&g);
        break;
    case T_FRECT:
        SetRect(&g, (short)(cx - 7), (short)(cy - 5), (short)(cx + 7), (short)(cy + 5));
        PaintRect(&g);
        break;
    case T_OVAL:
        SetRect(&g, (short)(cx - 7), (short)(cy - 5), (short)(cx + 7), (short)(cy + 5));
        FrameOval(&g);
        break;
    case T_FOVAL:
        SetRect(&g, (short)(cx - 7), (short)(cy - 5), (short)(cx + 7), (short)(cy + 5));
        PaintOval(&g);
        break;
    case T_FILL:                                /* bucket: triangle + drip */
        MoveTo((short)(cx - 6), cy); LineTo(cx, (short)(cy - 6));
        LineTo((short)(cx + 6), cy); LineTo(cx, (short)(cy + 6));
        LineTo((short)(cx - 6), cy);
        break;
    case T_SPRAY: {
        short k;
        for (k = 0; k < 7; k++) {
            short sx = (short)(r->left + 5 + ((k * 5) % 13));
            short sy = (short)(r->top + 5 + ((k * 7) % 13));
            MoveTo(sx, sy); LineTo(sx, sy);
        }
        break;
    }
    }
}

#if UNO_COLOR
#define PT_NSWATCH 14
static const unsigned char kPtQuick[PT_NSWATCH] = {
    /* black, white, primaries/secondaries, ramps */
    0, 255, 180, 30, 5, 210, 35, 185, 215, 223, 232, 237, 241, 246
};
#endif

static void pt_strip_rect(UnoWin *w, short i, Rect *r)
{
    r->left = (short)(w->bounds.left + PT_TOOLW + 34 + i * 26);
    r->top  = (short)(w->bounds.bottom - 28);
    r->right  = (short)(r->left + 22);
    r->bottom = (short)(r->top + 18);
}

static void pt_draw_strip(UnoWin *w)
{
    short i; Rect r;
#if UNO_COLOR
    for (i = 0; i < PT_NSWATCH; i++) {
        pt_strip_rect(w, i, &r);
        RGBForeColor(&kPtPal[kPtQuick[i]]);
        PaintRect(&r);
        RGBForeColor(&kBlack);
        if (gPtColor == kPtQuick[i]) { InsetRect(&r, -2, -2); uno_box(&r, C_WHITE); }
    }
    /* current color + "more" cell that opens the full picker */
    pt_strip_rect(w, PT_NSWATCH, &r);
    RGBForeColor(&kPtPal[gPtColor]); PaintRect(&r); RGBForeColor(&kBlack);
    uno_box(&r, C_WHITE);
    text_at((short)(r.right + 6), (short)(r.bottom - 4), "c: all colors",
            C_CYAN, C_BLUE, false);
#else
    for (i = 0; i < 10; i++) {
        Pattern p;
        pt_strip_rect(w, i, &r);
        memcpy(&p, kPtPats[i], 8);
        FillRect(&r, &p);
        FrameRect(&r);
        if (gPtPat == i) { InsetRect(&r, -2, -2); FrameRect(&r); }
    }
#endif
}

#if UNO_COLOR
static void pt_picker_cell(UnoWin *w, short i, Rect *r)
{
    Rect cr;
    pt_canvas_rect(w, &cr);
    r->left = (short)(cr.left + 28 + (i % 16) * 22);
    r->top  = (short)(cr.top + 12 + (i / 16) * 13);
    r->right  = (short)(r->left + 20);
    r->bottom = (short)(r->top + 11);
}

static void pt_draw_picker(UnoWin *w)
{
    short i; Rect r, cr;
    pt_canvas_rect(w, &cr);
    uno_fill(&cr, C_BLUE);
    for (i = 0; i < 256; i++) {
        pt_picker_cell(w, i, &r);
        RGBForeColor(&kPtPal[i]); PaintRect(&r); RGBForeColor(&kBlack);
        if (i == gPtColor) { InsetRect(&r, -2, -2); uno_box(&r, C_WHITE); }
    }
    text_at((short)(cr.left + 28), (short)(cr.bottom - 6),
            "every 8-bit color - click to pick, c: back", C_CYAN, C_BLUE, false);
}
#endif

static void pt_repaint_canvas(UnoWin *w)
{
    Rect cr;
    short y;
    pt_canvas_rect(w, &cr);
#if UNO_COLOR
    if (gPtPicker) { pt_draw_picker(w); return; }
#endif
    if (!gPtCanvas) return;
    /* run-length rows */
    for (y = 0; y < PT_H; y++) {
        short x = 0;
        unsigned char *row = gPtCanvas + (long)y * PT_W;
        while (x < PT_W) {
            short x0 = x;
            unsigned char v = row[x];
            Rect run;
            while (x < PT_W && row[x] == v) x++;
            SetRect(&run, (short)(cr.left + x0), (short)(cr.top + y),
                          (short)(cr.left + x),  (short)(cr.top + y + 1));
#if UNO_COLOR
            RGBForeColor(&kPtPal[v]); PaintRect(&run);
#else
            if (v) PaintRect(&run);
            else { PenMode(patBic); PaintRect(&run); PenNormal(); }
#endif
        }
    }
#if UNO_COLOR
    RGBForeColor(&kBlack);
#endif
}

static void paint_draw(UnoWin *w)
{
    Rect r = w->bounds, ct = r, cr;
    short i;
    ct.top += TBAR_H; InsetRect(&ct, 1, 1); uno_fill(&ct, C_BLUE);
    /* tool palette */
    for (i = 0; i < PT_TOOLS; i++) {
        Rect tr; pt_tool_rect(w, i, &tr);
        uno_fill(&tr, C_WHITE);
#if UNO_COLOR
        RGBForeColor(&kBlack);
#endif
        pt_draw_toolglyph(i, &tr);
        if (i == gPtTool) { InsetRect(&tr, -2, -2); uno_box(&tr, C_MAG); }
        else { uno_box(&tr, C_CYAN); }
    }
    /* canvas frame + content */
    pt_canvas_rect(w, &cr);
    InsetRect(&cr, -1, -1); uno_box(&cr, C_WHITE); InsetRect(&cr, 1, 1);
    pt_repaint_canvas(w);
    pt_draw_strip(w);
}

static void paint_open(void)
{
#if UNO_COLOR
    pt_build_palette();
#endif
    if (!gPtCanvas) {
        gPtCanvas = (unsigned char *)NewPtr((long)PT_W * PT_H);
        if (gPtCanvas) memset(gPtCanvas, PT_BG, (long)PT_W * PT_H);
    }
}

static void pt_save(void)
{
    short ref; long count = (long)PT_W * PT_H;
    OSErr err;
    if (!gPtCanvas) return;
    err = Create("\011PAINT.UNO", 0, 'UNOD', 'UPNT');
    if (err != noErr && err != dupFNErr) return;
    if (FSOpen("\011PAINT.UNO", 0, &ref) != noErr) return;
    FSWrite(ref, &count, (Ptr)gPtCanvas);
    FSClose(ref);
    FlushVol(NULL, 0);
}

static void pt_load(UnoWin *w)
{
    short ref; long count = (long)PT_W * PT_H;
    if (!gPtCanvas) return;
    if (FSOpen("\011PAINT.UNO", 0, &ref) != noErr) return;
    FSRead(ref, &count, (Ptr)gPtCanvas);
    FSClose(ref);
    if (w) draw_window(w);
}

static Boolean paint_key(char ch, short code)
{
    UnoWin *w = find_app_window(APP_PAINT);
    (void)code;
    if (ch >= '1' && ch <= '9') gPtTool = (short)(ch - '1');
    else if (ch == '0') gPtTool = T_SPRAY;
#if UNO_COLOR
    else if (ch == 'c') gPtPicker = !gPtPicker;
#endif
    else if (ch == 'n') { if (gPtCanvas) memset(gPtCanvas, PT_BG, (long)PT_W * PT_H); }
    else if (ch == 's') { pt_save(); return true; }
    else if (ch == 'l') { pt_load(w); return true; }
    else return false;
    if (w) draw_window(w);
    return true;
}

static void paint_click(UnoWin *w, Point p)
{
    Rect cr;
    short i;
    /* tool palette */
    for (i = 0; i < PT_TOOLS; i++) {
        Rect tr; pt_tool_rect(w, i, &tr);
        if (PtInRect(p, &tr)) { gPtTool = i; draw_window(w); return; }
    }
    /* strip */
#if UNO_COLOR
    for (i = 0; i <= PT_NSWATCH; i++) {
        Rect sr; pt_strip_rect(w, i, &sr);
        if (PtInRect(p, &sr)) {
            if (i == PT_NSWATCH) gPtPicker = !gPtPicker;
            else gPtColor = kPtQuick[i];
            draw_window(w);
            return;
        }
    }
#else
    for (i = 0; i < 10; i++) {
        Rect sr; pt_strip_rect(w, i, &sr);
        if (PtInRect(p, &sr)) { gPtPat = i; draw_window(w); return; }
    }
#endif
    pt_canvas_rect(w, &cr);
    if (!PtInRect(p, &cr) || !gPtCanvas) return;

#if UNO_COLOR
    if (gPtPicker) {                            /* picking from the grid */
        for (i = 0; i < 256; i++) {
            Rect pc; pt_picker_cell(w, i, &pc);
            if (PtInRect(p, &pc)) { gPtColor = (short)i; break; }
        }
        gPtPicker = false;
        draw_window(w);
        return;
    }
#endif
    {
        short x0 = (short)(p.h - cr.left), y0 = (short)(p.v - cr.top);
        short lx = x0, ly = y0;
        Point q;
        switch (gPtTool) {
        case T_PENCIL: case T_BRUSH: case T_ERASER: case T_SPRAY:
            for (;;) {
                GetMouse(&q);
                {
                    short x = (short)(q.h - cr.left), y = (short)(q.v - cr.top);
                    if (x < 0) x = 0; if (y < 0) y = 0;
                    if (x >= PT_W) x = PT_W - 1; if (y >= PT_H) y = PT_H - 1;
                    if (gPtTool == T_SPRAY) {
                        for (i = 0; i < 6; i++) {
                            short rx = (short)(x + (Random() % 11) - 5);
                            short ry = (short)(y + (Random() % 11) - 5);
                            if (rx >= 0 && ry >= 0 && rx < PT_W && ry < PT_H)
                                pt_set_px(w, rx, ry, pt_ink(rx, ry));
                        }
                    } else {
                        short sz = (short)(gPtTool == T_PENCIL ? 1 :
                                           gPtTool == T_BRUSH  ? 4 : 8);
                        /* connect drag gaps with a line of dots */
                        short sdx = (short)(x > lx ? x - lx : lx - x);
                        short sdy = (short)(y > ly ? y - ly : ly - y);
                        if (sdx > 1 || sdy > 1) {
                            short steps = (short)(sdx > sdy ? sdx : sdy), s2;
                            for (s2 = 1; s2 <= steps; s2++)
                                pt_dot(w, (short)(lx + (long)(x - lx) * s2 / steps),
                                          (short)(ly + (long)(y - ly) * s2 / steps),
                                       sz, gPtTool == T_ERASER);
                        } else {
                            pt_dot(w, x, y, sz, gPtTool == T_ERASER);
                        }
                        lx = x; ly = y;
                    }
                }
                if (!StillDown()) break;
            }
            break;
        case T_FILL:
            pt_flood(w, x0, y0);
            break;
        default: {                              /* rubber-band shapes */
            Rect band; short x1 = x0, y1 = y0;
            Boolean shown = false;
            PenMode(patXor);
            for (;;) {
                GetMouse(&q);
                {
                    short nx = (short)(q.h - cr.left), ny = (short)(q.v - cr.top);
                    if (nx < 0) nx = 0; if (ny < 0) ny = 0;
                    if (nx >= PT_W) nx = PT_W - 1; if (ny >= PT_H) ny = PT_H - 1;
                    if (nx != x1 || ny != y1 || !shown) {
                        if (shown) FrameRect(&band);    /* erase old */
                        x1 = nx; y1 = ny;
                        SetRect(&band,
                            (short)(cr.left + (x0 < x1 ? x0 : x1)),
                            (short)(cr.top  + (y0 < y1 ? y0 : y1)),
                            (short)(cr.left + (x0 > x1 ? x0 : x1) + 1),
                            (short)(cr.top  + (y0 > y1 ? y0 : y1) + 1));
                        FrameRect(&band);
                        shown = true;
                    }
                }
                if (!StillDown()) break;
            }
            if (shown) FrameRect(&band);
            PenNormal();
            switch (gPtTool) {
            case T_LINE:  pt_line(w, x0, y0, x1, y1, 1); break;
            case T_RECT:  pt_rect_shape(w, x0, y0, x1, y1, false); break;
            case T_FRECT: pt_rect_shape(w, x0, y0, x1, y1, true);  break;
            case T_OVAL:  pt_oval_shape(w, x0, y0, x1, y1, false); break;
            case T_FOVAL: pt_oval_shape(w, x0, y0, x1, y1, true);  break;
            }
            break;
        }
        }
    }
}

/* =========================================================================
 * True-color helpers - the color targets run 8-bit Color QuickDraw, so the
 * games use real RGB (nearest of the 256 system colors); the mono target
 * maps each entry to a 4-color theme slot for the 1-bit look.
 * ========================================================================= */
typedef struct { unsigned char r, g, b, mono; } GameRGB;

static void fill_rgb(Rect *q, const GameRGB *c)
{
#if UNO_COLOR
    RGBColor rc;
    rc.red   = (unsigned short)(c->r << 8);
    rc.green = (unsigned short)(c->g << 8);
    rc.blue  = (unsigned short)(c->b << 8);
    RGBForeColor(&rc);
    PaintRect(q);
    RGBForeColor(&kBlack);
#else
    uno_fill(q, c->mono);
#endif
}

/* =========================================================================
 * Game music - Korobeiniki (Dostris) + Sunset Drive (OutLast), parsed from
 * the x86 sources at port time. Shares the Sound Manager channel with the
 * Music app; muted while the owning game is not topmost.
 * ========================================================================= */
static const Note kKoro[] = { {76,16},{71,10},{72,10},{74,16},{72,10},{71,10},{69,16},{69,10},{72,10},{76,16},{74,10},{72,10},{71,26},{72,10},{74,16},{76,16},{72,16},{69,16},{69,33},{0,10},{74,26},{77,10},{81,16},{79,10},{77,10},{76,26},{72,10},{76,16},{74,10},{72,10},{71,16},{71,10},{72,10},{74,16},{76,16},{72,16},{69,16},{69,33},{0,10} };
#define N_KKORO (short)(sizeof(kKoro)/sizeof(kKoro[0]))
static const Note kDrive[] = { {76,10},{74,10},{72,20},{0,7},{76,10},{74,10},{72,10},{74,10},{76,20},{0,7},{74,10},{72,10},{71,20},{0,7},{74,10},{72,10},{71,10},{72,10},{74,20},{0,7},{72,20},{76,20},{79,20},{76,20},{74,20},{72,20},{71,20},{0,10},{72,20},{74,20},{76,40},{0,20} };
#define N_KDRIVE (short)(sizeof(kDrive)/sizeof(kDrive[0]))
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
    if (!(gZCount && zwin(gZCount - 1)->proc == gGmOwner)) return;
    if (TickCount() < gGmEnd) return;
    gGmIx++;
    if (gGmIx >= gGmCount) gGmIx = 0;
    gGmEnd = TickCount() + gGmNotes[gGmIx].dur;
    if (gGmNotes[gGmIx].midi) music_note_on(gGmNotes[gGmIx].midi, gGmNotes[gGmIx].dur);
    else music_quiet();
}

/* =========================================================================
 * Dostris - falling-blocks game (port of apps/dostris.asm)
 * Board 10x20, same piece tables / scoring / speed curve as the x86 game.
 * x86 gravity is in 18.2 Hz ticks; Mac ticks are 60 Hz, so intervals are
 * scaled by ~3.3 (x86_ticks * 10 / 3).
 * ========================================================================= */
#define DT_COLS 10
#define DT_ROWS 20
#define DT_CELL 16
#define DT_BX   10                  /* board origin inside the content area */
#define DT_BY   8

/* 7 pieces x 4 rotations x 4 cells x (col,row) - from apps/dostris.asm */
static const signed char kDtShape[7][4][8] = {
  { {0,1,1,1,2,1,3,1}, {2,0,2,1,2,2,2,3}, {0,2,1,2,2,2,3,2}, {1,0,1,1,1,2,1,3} },
  { {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1} },
  { {1,0,0,1,1,1,2,1}, {0,0,0,1,1,1,0,2}, {0,0,1,0,2,0,1,1}, {1,0,0,1,1,1,1,2} },
  { {1,0,2,0,0,1,1,1}, {0,0,0,1,1,1,1,2}, {1,0,2,0,0,1,1,1}, {0,0,0,1,1,1,1,2} },
  { {0,0,1,0,1,1,2,1}, {1,0,0,1,1,1,0,2}, {0,0,1,0,1,1,2,1}, {1,0,0,1,1,1,0,2} },
  { {0,0,0,1,1,1,2,1}, {0,0,1,0,0,1,0,2}, {0,0,1,0,2,0,2,1}, {1,0,1,1,0,2,1,2} },
  { {2,0,0,1,1,1,2,1}, {0,0,0,1,0,2,1,2}, {0,0,1,0,2,0,0,1}, {0,0,1,0,1,1,1,2} },
};
static const short kDtColor[7] = { C_CYAN, C_WHITE, C_MAG, C_CYAN, C_MAG, C_WHITE, C_CYAN };
/* VGA-variant piece colors (apps/tetrisv.asm look): I cyan, O yellow,
   T purple, S green, Z red, J blue, L orange */
static const GameRGB kDtRGB[7] = {
    {  0, 220, 220, C_CYAN  },      /* I */
    { 235, 215,   0, C_WHITE },     /* O */
    { 160,  60, 220, C_MAG   },     /* T */
    {  40, 200,  60, C_CYAN  },     /* S */
    { 230,  50,  50, C_MAG   },     /* Z */
    {  60, 100, 240, C_WHITE },     /* J */
    { 240, 150,  40, C_CYAN  },     /* L */
};
static const long kDtLineScore[5] = { 0, 40, 100, 300, 1200 };

static unsigned char gDtBoard[DT_ROWS][DT_COLS];
static short gDtState = 0;          /* 0 menu, 1 playing, 2 paused, 3 over */
static short gDtPiece, gDtRot, gDtCol, gDtRow, gDtNext;
static long  gDtScore, gDtLines;
static short gDtLevel;
static long  gDtLastDrop;
static unsigned long gDtSeed = 1;

static short dt_rand7(void)
{
    gDtSeed = gDtSeed * 1103515245UL + 12345UL;
    return (short)((gDtSeed >> 16) % 7);
}

static Boolean dt_fits(short p, short rot, short col, short row)
{
    short i;
    const signed char *sh = kDtShape[p][rot];
    for (i = 0; i < 4; i++) {
        short c = col + sh[i * 2], r = row + sh[i * 2 + 1];
        if (c < 0 || c >= DT_COLS || r >= DT_ROWS) return false;
        if (r >= 0 && gDtBoard[r][c]) return false;
    }
    return true;
}

static long dt_drop_interval(void)
{
    short t = 18 - gDtLevel;        /* x86 ticks (18.2 Hz) */
    if (t < 2) t = 2;
    return (long)t * 10 / 3;        /* -> 60 Hz ticks */
}

static void dt_spawn(void)
{
    gDtPiece = gDtNext;
    gDtNext = dt_rand7();
    gDtRot = 0; gDtCol = 3; gDtRow = -1;
    gDtLastDrop = TickCount();
    if (!dt_fits(gDtPiece, gDtRot, gDtCol, gDtRow + 1)) {
        gDtState = 3;               /* game over */
        gm_stop();
    }
}

static void dt_new_game(void)
{
    memset(gDtBoard, 0, sizeof(gDtBoard));
    gDtScore = 0; gDtLines = 0; gDtLevel = 1;
    gDtSeed = (unsigned long)TickCount() | 1;
    gDtNext = dt_rand7();
    gDtState = 1;
    dt_spawn();
    gm_start(kKoro, N_KKORO, APP_DOSTRIS);
}

static void dt_clear_lines(void)
{
    short r, c, n = 0;
    for (r = 0; r < DT_ROWS; r++) {
        Boolean full = true;
        for (c = 0; c < DT_COLS; c++)
            if (!gDtBoard[r][c]) { full = false; break; }
        if (full) {
            short rr;
            n++;
            for (rr = r; rr > 0; rr--)
                memcpy(gDtBoard[rr], gDtBoard[rr - 1], DT_COLS);
            memset(gDtBoard[0], 0, DT_COLS);
        }
    }
    if (n) {
        gDtScore += kDtLineScore[n] * (gDtLevel + 1);
        gDtLines += n;
        gDtLevel = (short)(gDtLines / 10) + 1;
        if (gDtLevel > 15) gDtLevel = 15;
    }
}

static void dt_lock(void)
{
    short i;
    const signed char *sh = kDtShape[gDtPiece][gDtRot];
    for (i = 0; i < 4; i++) {
        short c = gDtCol + sh[i * 2], r = gDtRow + sh[i * 2 + 1];
        if (r >= 0 && r < DT_ROWS && c >= 0 && c < DT_COLS)
            gDtBoard[r][c] = (unsigned char)(gDtPiece + 1);
    }
    dt_clear_lines();
    dt_spawn();
}

static void dt_cell(UnoWin *w, short c, short r, short piece)
{
    Rect q;
    short x = w->bounds.left + DT_BX + c * DT_CELL;
    short y = w->bounds.top + TBAR_H + DT_BY + r * DT_CELL;
    SetRect(&q, x, y, x + DT_CELL - 1, y + DT_CELL - 1);
    fill_rgb(&q, &kDtRGB[piece]);
#if UNO_COLOR
    {   /* bevel highlight for depth */
        Rect h = q;
        h.bottom = h.top + 2;
        RGBForeColor(&kPalette[C_WHITE]);
        PaintRect(&h);
        RGBForeColor(&kBlack);
    }
#endif
}

static void dostris_draw(UnoWin *w)
{
    Rect r = w->bounds, b;
    short c, rr, i, px;
    char num[16];

    /* board frame + cells */
    SetRect(&b, r.left + DT_BX - 2, r.top + TBAR_H + DT_BY - 2,
            r.left + DT_BX + DT_COLS * DT_CELL + 1,
            r.top + TBAR_H + DT_BY + DT_ROWS * DT_CELL + 1);
    uno_box(&b, C_WHITE);
    for (rr = 0; rr < DT_ROWS; rr++)
        for (c = 0; c < DT_COLS; c++)
            if (gDtBoard[rr][c])
                dt_cell(w, c, rr, gDtBoard[rr][c] - 1);
    if (gDtState == 1 || gDtState == 2) {
        const signed char *sh = kDtShape[gDtPiece][gDtRot];
        for (i = 0; i < 4; i++) {
            short cc = gDtCol + sh[i * 2], cr = gDtRow + sh[i * 2 + 1];
            if (cr >= 0) dt_cell(w, cc, cr, gDtPiece);
        }
    }

    /* side panel */
    px = r.left + DT_BX + DT_COLS * DT_CELL + 14;
    text_at(px, r.top + TBAR_H + 20, "DOSTRIS", C_MAG, C_BLUE, false);
    text_at(px, r.top + TBAR_H + 44, "Score", C_CYAN, C_BLUE, false);
    fmt_u(gDtScore, num);
    text_at(px + 56, r.top + TBAR_H + 44, num, C_WHITE, C_BLUE, false);
    text_at(px, r.top + TBAR_H + 60, "Lines", C_CYAN, C_BLUE, false);
    fmt_u(gDtLines, num);
    text_at(px + 56, r.top + TBAR_H + 60, num, C_WHITE, C_BLUE, false);
    text_at(px, r.top + TBAR_H + 76, "Level", C_CYAN, C_BLUE, false);
    fmt_u(gDtLevel, num);
    text_at(px + 56, r.top + TBAR_H + 76, num, C_WHITE, C_BLUE, false);

    text_at(px, r.top + TBAR_H + 100, "Next", C_CYAN, C_BLUE, false);
    SetRect(&b, px - 2, r.top + TBAR_H + 106,
            px + 4 * 10 + 2, r.top + TBAR_H + 106 + 4 * 10 + 2);
    uno_box(&b, C_WHITE);
    {
        const signed char *sh = kDtShape[gDtNext][0];
        for (i = 0; i < 4; i++) {
            Rect q;
            short x = px + sh[i * 2] * 10;
            short y = r.top + TBAR_H + 108 + sh[i * 2 + 1] * 10;
            SetRect(&q, x, y, x + 9, y + 9);
            if (gDtState != 0) fill_rgb(&q, &kDtRGB[gDtNext]);
        }
    }

    text_at(px, r.bottom - 56, "Arrows: move/rot", C_CYAN, C_BLUE, false);
    text_at(px, r.bottom - 42, "Space: drop", C_CYAN, C_BLUE, false);
    text_at(px, r.bottom - 28, "P: pause  N: new", C_CYAN, C_BLUE, false);
    if (gDtState == 0)
        text_at(px, r.bottom - 12, "N: new game", C_WHITE, C_BLUE, false);
    else if (gDtState == 2)
        text_at(px, r.bottom - 12, "PAUSED", C_MAG, C_BLUE, false);
    else if (gDtState == 3)
        text_at(px, r.bottom - 12, "GAME OVER", C_MAG, C_BLUE, false);
}

static void dt_redraw(void)
{
    UnoWin *w = find_app_window(APP_DOSTRIS);
    if (w && gZCount && zwin(gZCount - 1) == w) draw_window(w);
}

static Boolean dostris_key(char ch, short code)
{
    if (ch == 'n' || ch == 'N') { dt_new_game(); dt_redraw(); return true; }
    if (gDtState != 1) {
        if (ch == 'p' || ch == 'P') {
            if (gDtState == 2) {
                gDtState = 1; gDtLastDrop = TickCount();
                gm_start(kKoro, N_KKORO, APP_DOSTRIS);
                dt_redraw();
            }
            return true;
        }
        return (ch == ' ');
    }
    if (ch == 'p' || ch == 'P') { gDtState = 2; gm_stop(); dt_redraw(); return true; }
    if (code == 0x7B || ch == 0x1C) {                   /* left */
        if (dt_fits(gDtPiece, gDtRot, gDtCol - 1, gDtRow)) gDtCol--;
        dt_redraw(); return true;
    }
    if (code == 0x7C || ch == 0x1D) {                   /* right */
        if (dt_fits(gDtPiece, gDtRot, gDtCol + 1, gDtRow)) gDtCol++;
        dt_redraw(); return true;
    }
    if (code == 0x7E || ch == 0x1E) {                   /* up: rotate */
        short nr = (short)((gDtRot + 1) & 3);
        if (dt_fits(gDtPiece, nr, gDtCol, gDtRow)) gDtRot = nr;
        dt_redraw(); return true;
    }
    if (code == 0x7D || ch == 0x1F) {                   /* down: soft drop */
        if (dt_fits(gDtPiece, gDtRot, gDtCol, gDtRow + 1)) {
            gDtRow++; gDtScore++;
            gDtLastDrop = TickCount();
        } else dt_lock();
        dt_redraw(); return true;
    }
    if (ch == ' ') {                                    /* hard drop */
        while (dt_fits(gDtPiece, gDtRot, gDtCol, gDtRow + 1)) {
            gDtRow++; gDtScore += 2;
        }
        dt_lock();
        dt_redraw(); return true;
    }
    return false;
}

static void dostris_tick(void)
{
    if (gDtState != 1) return;
    if (!(gZCount && zwin(gZCount - 1)->proc == APP_DOSTRIS)) return;
    if (TickCount() - gDtLastDrop < dt_drop_interval()) return;
    gDtLastDrop = TickCount();
    if (dt_fits(gDtPiece, gDtRot, gDtCol, gDtRow + 1)) gDtRow++;
    else dt_lock();
    dt_redraw();
}

/* =========================================================================
 * OutLast - pseudo-3D racer (port of apps/outlast.asm)
 * Same track table, perspective math, traffic and physics; the x86 18.2 Hz
 * tick constants are scaled to 60 Hz where they are time-based.
 * ========================================================================= */
#define OL_W       320              /* virtual playfield; rendered at 3/2 */
#define OL_H       200
#define OLS(v)     ((short)(((v) * 3) / 2))
#define OL_HORIZON 80
#define OL_SEGLEN  80
#define OL_NSEG    32
#define OL_TRACKLEN (OL_SEGLEN * OL_NSEG)

static const signed char kOlCurve[OL_NSEG] = {
    0,0,0,0,0,0,0,0,  5,15,25,30,  30,25,15,5,
    0,0,0,0,0,0,0,0,  -5,-15,-25,-30,  -30,-25,-15,-5
};
static const short kOlTreeZ[8] = { 200, 520, 900, 1300, 1600, 1900, 2200, 2480 };

static short gOlState = 0;          /* 0 title, 1 playing, 2 game over */
static short gOlX = 160, gOlSpeed = 0;
static long  gOlZ = 0, gOlScore = 0;
static short gOlTime = 60, gOlCrash = 0;
static long  gOlLastStep, gOlLastSec;
static long  gOlTraffic[4];         /* z along track */
static const unsigned char kOlTrafDir[4]  = { 1, 1, 0, 0 };  /* 1 = same dir */
static const unsigned char kOlTrafLane[4] = { 0, 1, 0, 1 };
static short gOlRoadL = 100, gOlRoadR = 220;   /* road edges at the car row */

static void ol_new_game(void)
{
    gOlX = 160; gOlSpeed = 0; gOlZ = 0; gOlScore = 0;
    gOlTime = 60; gOlCrash = 0;
    gOlTraffic[0] = 400; gOlTraffic[1] = 1600;
    gOlTraffic[2] = 800; gOlTraffic[3] = 2000;
    gOlLastStep = gOlLastSec = TickCount();
    gOlState = 1;
    gm_start(kDrive, N_KDRIVE, APP_OUTLAST);
}

/* OutLast true-color set (apps/outlastv.asm look); mono slot fallback */
enum { OC_SKY = 0, OC_HORIZON, OC_GRASS_A, OC_GRASS_B, OC_ROAD, OC_ROAD_B,
       OC_STRIPE, OC_CAR, OC_CARWIN, OC_WHEEL, OC_TRAF_ON, OC_TRAF_SAME,
       OC_TRUNK, OC_CANOPY, OC_HUD, OC_NCOLORS };
static const GameRGB kOlRGB[OC_NCOLORS] = {
    { 110, 170, 240, C_BLUE  },     /* sky        */
    { 250, 200, 120, C_CYAN  },     /* horizon haze */
    {  51, 153,  51, C_CYAN  },     /* grass A (palette-cube safe) */
    {  30, 120,  40, C_MAG   },     /* grass B    */
    { 110, 110, 110, C_WHITE },     /* road       */
    { 100, 100, 100, C_WHITE },     /* road alt   */
    { 240, 220,  60, C_BLUE  },     /* stripe     */
    { 220,  40,  40, C_MAG   },     /* player car */
    { 140, 220, 240, C_CYAN  },     /* windshield */
    {  25,  25,  25, C_BLUE  },     /* wheels     */
    { 245, 245, 245, C_WHITE },     /* oncoming   */
    { 240, 200,  60, C_CYAN  },     /* same-dir   */
    { 120,  80,  40, C_BLUE  },     /* trunk      */
    {  30, 140,  45, C_CYAN  },     /* canopy     */
    {  10,  10,  40, C_BLUE  },     /* HUD bar    */
};

static void ol_vrect(UnoWin *w, short x0, short y0, short x1, short y1, short col)
{
    Rect q;
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < 0) x0 = 0;
    if (x1 > OL_W) x1 = OL_W;
    SetRect(&q, w->bounds.left + 4 + OLS(x0), w->bounds.top + TBAR_H + 2 + OLS(y0),
            w->bounds.left + 4 + OLS(x1), w->bounds.top + TBAR_H + 2 + OLS(y1));
    fill_rgb(&q, &kOlRGB[col]);
}

static void outlast_draw(UnoWin *w)
{
    short y;
    long dx = 0;
    char num[16], hud[48];

    if (gOlState == 0) {
        ol_vrect(w, 0, 0, OL_W, 100, OC_SKY);
        ol_vrect(w, 0, 100, OL_W, 102, OC_HORIZON);
        ol_vrect(w, 0, 102, OL_W, OL_H, OC_GRASS_A);
        for (y = 0; y < 10; y++) {                  /* converging road bands */
            short t = (short)(102 + y * 10);
            short hw2 = (short)(8 + y * 14);
            ol_vrect(w, 160 - hw2, t, 160 + hw2, t + 10, OC_ROAD);
        }
        ol_vrect(w, 140, 150, 180, 176, OC_CAR);    /* car silhouette */
        ol_vrect(w, 146, 154, 174, 162, OC_CARWIN);
        ol_vrect(w, 136, 170, 144, 178, OC_WHEEL);
        ol_vrect(w, 176, 170, 184, 178, OC_WHEEL);
        text_at(w->bounds.left + 4 + OLS(120), w->bounds.top + TBAR_H + 2 + OLS(30),
                "O U T L A S T", C_WHITE, C_BLUE, false);
        text_at(w->bounds.left + 4 + OLS(112), w->bounds.top + TBAR_H + 2 + OLS(190),
                "Press N to drive", C_CYAN, C_BLUE, false);
        return;
    }

    /* sky */
    ol_vrect(w, 0, 12, OL_W, OL_HORIZON, OC_SKY);
    ol_vrect(w, 0, OL_HORIZON, OL_W, OL_HORIZON + 2, OC_HORIZON);

    /* road strips, bottom to top */
    for (y = OL_H - 1; y > OL_HORIZON + 1; y -= 2) {
        long z = 4800L / (y - OL_HORIZON);
        long hw = (16L * 256L) / z;
        long worldz = gOlZ + z * 4;
        short seg = (short)((worldz / OL_SEGLEN) & (OL_NSEG - 1));
        short center;
        short l, rgt;
        dx += kOlCurve[seg];
        center = (short)(160 + (dx >> 5));
        l = (short)(center - hw); rgt = (short)(center + hw);
        ol_vrect(w, 0, y - 2, l, y, (seg & 1) ? OC_GRASS_B : OC_GRASS_A);
        ol_vrect(w, l, y - 2, rgt, y, (seg & 1) ? OC_ROAD_B : OC_ROAD);
        ol_vrect(w, rgt, y - 2, OL_W, y, (seg & 1) ? OC_GRASS_B : OC_GRASS_A);
        if (seg & 1)                                /* center stripe */
            ol_vrect(w, center - 2, y - 2, center + 2, y, OC_STRIPE);
        if (y >= OL_H - 4) { gOlRoadL = l; gOlRoadR = rgt; }
        /* trees near this strip */
        {
            short t;
            for (t = 0; t < 8; t++) {
                long rel = kOlTreeZ[t] - (gOlZ % OL_TRACKLEN);
                if (rel < 0) rel += OL_TRACKLEN;
                if (rel < 30 || rel > 400) continue;
                if (rel >= z * 4 - 8 && rel < z * 4 + 8) {
                    short th = (short)(1600 / (rel ? rel : 1));
                    short tw = (short)(800 / (rel ? rel : 1));
                    short tx;
                    if (th < 2) th = 2; if (th > 40) th = 40;
                    if (tw < 2) tw = 2; if (tw > 24) tw = 24;
                    tx = (t & 1) ? (short)(rgt + 4) : (short)(l - 4 - tw);
                    ol_vrect(w, (short)(tx + tw / 2 - tw / 8), (short)(y - th / 2), (short)(tx + tw / 2 + tw / 8), y, OC_TRUNK);
                    ol_vrect(w, tx, (short)(y - th), (short)(tx + tw), (short)(y - th / 2), OC_CANOPY);
                }
            }
        }
    }

    /* traffic */
    {
        short t;
        for (t = 0; t < 4; t++) {
            long rel = gOlTraffic[t] - (gOlZ % OL_TRACKLEN);
            if (rel < 0) rel += OL_TRACKLEN;
            if (rel >= 10 && rel <= 400) {
                short ch2 = (short)(1500 / rel), cw = (short)(2100 / rel);
                short cy, cx;
                if (ch2 < 2) ch2 = 2; if (ch2 > 40) ch2 = 40;
                if (cw < 3) cw = 3; if (cw > 50) cw = 50;
                cy = (short)(OL_HORIZON + 4800 / (rel / 4 + 25));
                if (cy > OL_H - 6) cy = OL_H - 6;
                cx = (short)(160 + (kOlTrafLane[t] ? 30 : -30) * (200 - (short)(rel / 2)) / 200);
                ol_vrect(w, (short)(cx - cw / 2), (short)(cy - ch2), (short)(cx + cw / 2), cy,
                         kOlTrafDir[t] ? OC_TRAF_SAME : OC_TRAF_ON);
                ol_vrect(w, (short)(cx - cw / 2 + 1), (short)(cy - ch2), (short)(cx + cw / 2 - 1),
                         (short)(cy - ch2 + (ch2 / 4 ? ch2 / 4 : 1)), OC_WHEEL);
            }
        }
    }

    /* player car at y=168 */
    if (!(gOlCrash & 4)) {                          /* flash while crashed */
        ol_vrect(w, gOlX - 14, 168, gOlX + 14, 186, OC_CAR);
        ol_vrect(w, gOlX - 10, 171, gOlX + 10, 177, OC_CARWIN);
        ol_vrect(w, gOlX - 16, 182, gOlX - 10, 190, OC_WHEEL);
        ol_vrect(w, gOlX + 10, 182, gOlX + 16, 190, OC_WHEEL);
    }

    /* HUD */
    ol_vrect(w, 0, 0, OL_W, 12, OC_HUD);
    strcpy(hud, "Speed ");  fmt_u(gOlSpeed, num); strcat(hud, num);
    strcat(hud, "  Score "); fmt_u(gOlScore, num); strcat(hud, num);
    strcat(hud, "  Time ");  fmt_u(gOlTime, num);  strcat(hud, num);
    text_at(w->bounds.left + 8, w->bounds.top + TBAR_H + 14, hud,
            C_WHITE, C_BLUE, false);

    if (gOlState == 2) {
        ol_vrect(w, 80, 70, 240, 130, OC_HUD);
        text_at(w->bounds.left + 4 + OLS(124), w->bounds.top + TBAR_H + 2 + OLS(92),
                "GAME OVER", C_WHITE, C_BLUE, false);
        strcpy(hud, "Final score "); fmt_u(gOlScore, num); strcat(hud, num);
        text_at(w->bounds.left + 4 + OLS(104), w->bounds.top + TBAR_H + 2 + OLS(108),
                hud, C_CYAN, C_BLUE, false);
        text_at(w->bounds.left + 4 + OLS(110), w->bounds.top + TBAR_H + 2 + OLS(122),
                "N: new game", C_CYAN, C_BLUE, false);
    }
}

static Boolean outlast_key(char ch, short code)
{
    if (ch == 'n' || ch == 'N') {
        ol_new_game();
        return true;
    }
    if (gOlState != 1 || gOlCrash) return false;
    if (code == 0x7B || ch == 0x1C) { gOlX -= 9; if (gOlX < 40) gOlX = 40; return true; }
    if (code == 0x7C || ch == 0x1D) { gOlX += 9; if (gOlX > 280) gOlX = 280; return true; }
    if (code == 0x7E || ch == 0x1E) { gOlSpeed += 4; if (gOlSpeed > 60) gOlSpeed = 60; return true; }
    if (code == 0x7D || ch == 0x1F) { gOlSpeed -= 8; if (gOlSpeed < 0) gOlSpeed = 0; return true; }
    return false;
}

static void outlast_tick(void)
{
    UnoWin *w;
    long now = TickCount();
    if (gOlState != 1) return;
    if (!(gZCount && zwin(gZCount - 1)->proc == APP_OUTLAST)) return;
    if (now - gOlLastStep < 4) return;              /* ~15 fps */
    gOlLastStep = now;

    if (gOlCrash) {
        gOlCrash--;
        if (!gOlCrash) { gOlX = 160; gOlSpeed = 5; }
    } else {
        short seg = (short)((gOlZ / OL_SEGLEN) & (OL_NSEG - 1));
        if (gOlSpeed < 60) gOlSpeed++;
        if (gOlX < gOlRoadL || gOlX > gOlRoadR) {   /* grass */
            gOlSpeed -= 2;
            if (gOlSpeed < 5) gOlSpeed = 5;
        }
        gOlX -= kOlCurve[seg] / 8;                  /* curve drift */
        if (gOlX < 40) gOlX = 40;
        if (gOlX > 280) gOlX = 280;
        gOlZ += gOlSpeed;
        gOlScore += gOlSpeed >> 2;
        /* traffic movement + collision */
        {
            short t;
            for (t = 0; t < 4; t++) {
                if (kOlTrafDir[t]) gOlTraffic[t] += 5;
                else               gOlTraffic[t] -= 5;
                if (gOlTraffic[t] < 0) gOlTraffic[t] += OL_TRACKLEN;
                if (gOlTraffic[t] >= OL_TRACKLEN) gOlTraffic[t] -= OL_TRACKLEN;
                {
                    long rel = gOlTraffic[t] - (gOlZ % OL_TRACKLEN);
                    if (rel < 0) rel += OL_TRACKLEN;
                    if (rel < 15) {
                        short cx = (short)(160 + (kOlTrafLane[t] ? 30 : -30));
                        if (gOlX > cx - 25 && gOlX < cx + 25) gOlCrash = 30;
                    }
                }
            }
        }
        /* tree collision: off-road at a tree's z */
        {
            short t;
            for (t = 0; t < 8; t++) {
                long rel = kOlTreeZ[t] - (gOlZ % OL_TRACKLEN);
                if (rel < 0) rel += OL_TRACKLEN;
                if (rel < 12 &&
                    ((t & 1) ? (gOlX > gOlRoadR - 5) : (gOlX < gOlRoadL + 5)))
                    gOlCrash = 30;
            }
        }
        if (gOlCrash) gOlSpeed = 0;
    }

    /* 1-second timer */
    if (now - gOlLastSec >= 60) {
        gOlLastSec = now;
        if (--gOlTime <= 0) { gOlTime = 0; gOlState = 2; gm_stop(); }
    }

    w = find_app_window(APP_OUTLAST);
    if (w) draw_window(w);
}

/* =========================================================================
 * Pac-Man - port of apps/pacman.asm. 28x25 tile maze (8px tiles), three
 * ghosts (Blinky chase-direct, Pinky 4-ahead, Clyde hybrid), scatter/chase
 * schedule, power pellets with frightened mode and 200/400/800/1600 chain.
 * ========================================================================= */
#define PM_COLS 28
#define PM_ROWS 25
#define PM_TILE 8

/* tile codes: 0 empty, 1 wall, 2 dot, 3 power, 4 house, 5 gate */
static const unsigned char kPmMaze[PM_ROWS][PM_COLS] = {
 {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
 {1,3,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,3,1},
 {1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1},
 {1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1},
 {1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1},
 {1,2,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,2,1},
 {1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1},
 {1,1,1,1,1,2,1,1,1,1,0,1,0,0,0,0,1,0,1,1,1,1,2,1,1,1,1,1},
 {0,0,0,0,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0},
 {0,0,0,0,1,2,1,0,1,1,1,1,5,0,0,5,1,1,1,1,0,1,2,1,0,0,0,0},
 {1,1,1,1,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,1,1,1,1},
 {0,0,0,0,0,2,0,0,1,4,4,4,4,4,4,4,4,4,4,1,0,0,2,0,0,0,0,0},
 {0,0,0,0,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,0,0,0,0},
 {0,0,0,0,1,2,1,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1,2,1,0,0,0,0},
 {1,1,1,1,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,1,1,1,1},
 {1,2,2,2,2,2,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,2,2,2,2,2,1},
 {1,2,1,1,1,2,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,2,1,1,1,2,1},
 {1,3,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,3,1},
 {1,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,1},
 {1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1},
 {1,2,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,2,1},
 {1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1},
 {1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1},
 {1,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,1},
 {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

enum { PM_TITLE = 0, PM_READY, PM_PLAY, PM_DEAD, PM_OVER, PM_LEVELUP };
enum { GH_HOUSE = 0, GH_SCATTER, GH_CHASE, GH_FRIGHT, GH_EATEN };
enum { D_UP = 0, D_LEFT, D_DOWN, D_RIGHT, D_NONE };

typedef struct { short x, y, dir, state, timer; } PmGhost;

static unsigned char gPmMaze[PM_ROWS][PM_COLS];
static short gPmState = PM_TITLE;
static short gPmX, gPmY, gPmDir, gPmNextDir;
static PmGhost gPmGh[3];
static long  gPmScore, gPmHi;
static short gPmLives, gPmLevel, gPmDots;
static short gPmMode, gPmFright, gPmKills;
static long  gPmModeT, gPmLastStep, gPmStateT;
static unsigned long gPmSeed = 7;

static const short kPmModeDur[8] = { 127, 364, 127, 364, 91, 364, 91, 0x7FFF };
static const short kPmDX[4] = { 0, -1, 0, 1 };
static const short kPmDY[4] = { -1, 0, 1, 0 };
/* scatter corners: blinky TR, pinky TL, clyde BL */
static const short kPmCornX[3] = { 26, 1, 1 };
static const short kPmCornY[3] = { 1, 1, 23 };

static short pm_rand(short n)
{
    gPmSeed = gPmSeed * 1103515245UL + 12345UL;
    return (short)((gPmSeed >> 16) % n);
}

static Boolean pm_walkable(short tx, short ty, short forGhost, short eaten)
{
    unsigned char t;
    if (ty < 0 || ty >= PM_ROWS) return false;
    if (tx < 0) tx += PM_COLS;
    if (tx >= PM_COLS) tx -= PM_COLS;
    t = gPmMaze[ty][tx];
    if (t == 1) return false;
    if (t == 4) return forGhost && eaten;
    if (t == 5) return forGhost && eaten;
    return true;
}

static void pm_reset_actors(void)
{
    gPmX = 14 * PM_TILE; gPmY = 19 * PM_TILE;
    gPmDir = D_LEFT; gPmNextDir = D_LEFT;
    gPmGh[0].x = 14 * PM_TILE; gPmGh[0].y = 10 * PM_TILE;
    gPmGh[0].dir = D_LEFT; gPmGh[0].state = GH_SCATTER; gPmGh[0].timer = 0;
    gPmGh[1].x = 13 * PM_TILE; gPmGh[1].y = 12 * PM_TILE;
    gPmGh[1].dir = D_UP; gPmGh[1].state = GH_HOUSE; gPmGh[1].timer = 100;
    gPmGh[2].x = 15 * PM_TILE; gPmGh[2].y = 12 * PM_TILE;
    gPmGh[2].dir = D_UP; gPmGh[2].state = GH_HOUSE; gPmGh[2].timer = 200;
    gPmFright = 0; gPmKills = 0;
}

static void pm_load_maze(void)
{
    short r, c;
    gPmDots = 0;
    for (r = 0; r < PM_ROWS; r++)
        for (c = 0; c < PM_COLS; c++) {
            gPmMaze[r][c] = kPmMaze[r][c];
            if (kPmMaze[r][c] == 2 || kPmMaze[r][c] == 3) gPmDots++;
        }
}

static void pm_new_game(void)
{
    gPmScore = 0; gPmLives = 3; gPmLevel = 1;
    gPmMode = 0; gPmModeT = 0;
    pm_load_maze();
    pm_reset_actors();
    gPmState = PM_READY;
    gPmStateT = TickCount() + 66;
    gPmLastStep = TickCount();
    gPmSeed = (unsigned long)TickCount() | 1;
}

static short pm_mode_state(void)
{
    return (gPmMode & 1) ? GH_CHASE : GH_SCATTER;
}

/* pick the ghost's direction at a tile center */
static void pm_ghost_steer(short gi)
{
    PmGhost *g = &gPmGh[gi];
    short gtx = g->x / PM_TILE, gty = g->y / PM_TILE;
    short ptx = gPmX / PM_TILE, pty = gPmY / PM_TILE;
    short tx, ty, best = -1, bestd = 0x7FFF, d, rev;

    if (g->state == GH_FRIGHT) {
        short tries = 8;
        rev = g->dir ^ 2;
        while (tries--) {
            d = pm_rand(4);
            if (d == rev) continue;
            if (pm_walkable(gtx + kPmDX[d], gty + kPmDY[d], 1, 0)) { g->dir = d; return; }
        }
        return;
    }
    if (g->state == GH_EATEN) { tx = 14; ty = 10; }
    else if (g->state == GH_CHASE) {
        if (gi == 0) { tx = ptx; ty = pty; }
        else if (gi == 1) {
            tx = ptx + kPmDX[gPmDir] * 4; ty = pty + kPmDY[gPmDir] * 4;
        } else {
            short md = (short)((gtx > ptx ? gtx - ptx : ptx - gtx) +
                               (gty > pty ? gty - pty : pty - gty));
            if (md <= 8) { tx = 1; ty = 1; }
            else { tx = ptx; ty = pty; }
        }
    } else { tx = kPmCornX[gi]; ty = kPmCornY[gi]; }

    rev = g->dir ^ 2;                   /* UP<->DOWN, LEFT<->RIGHT */
    for (d = 0; d < 4; d++) {
        short nx, ny;
        if (d == rev) continue;
        nx = gtx + kPmDX[d]; ny = gty + kPmDY[d];
        if (!pm_walkable(nx, ny, 1, g->state == GH_EATEN)) continue;
        if (nx < 0) nx += PM_COLS;
        if (nx >= PM_COLS) nx -= PM_COLS;
        {
            short ddx = nx > tx ? nx - tx : tx - nx;
            short ddy = ny > ty ? ny - ty : ty - ny;
            short dist = (short)(ddx + ddy);
            if (dist < bestd) { bestd = dist; best = d; }
        }
    }
    if (best >= 0) g->dir = best;
}

static void pm_kill_pac(void)
{
    gPmLives--;
    if (gPmLives <= 0) {
        gPmState = PM_OVER;
        if (gPmScore > gPmHi) gPmHi = gPmScore;
        gm_stop();
    } else {
        pm_reset_actors();
        gPmState = PM_READY;
        gPmStateT = TickCount() + 66;
    }
}

static void pm_step(void)
{
    short i, sub;

    /* mode schedule (frozen while frightened) */
    if (gPmFright > 0) {
        gPmFright--;
        if (!gPmFright)
            for (i = 0; i < 3; i++)
                if (gPmGh[i].state == GH_FRIGHT) gPmGh[i].state = pm_mode_state();
    } else {
        gPmModeT++;
        if (gPmModeT >= kPmModeDur[gPmMode > 7 ? 7 : gPmMode]) {
            gPmModeT = 0;
            if (gPmMode < 7) gPmMode++;
            for (i = 0; i < 3; i++)
                if (gPmGh[i].state == GH_SCATTER || gPmGh[i].state == GH_CHASE) {
                    gPmGh[i].state = pm_mode_state();
                    gPmGh[i].dir ^= 2;          /* reverse on mode change */
                }
        }
    }

    for (sub = 0; sub < 2; sub++) {             /* 2px per step */
        /* --- pac --- */
        if ((gPmX % PM_TILE) == 0 && (gPmY % PM_TILE) == 0) {
            short tx = gPmX / PM_TILE, ty = gPmY / PM_TILE;
            unsigned char *t = &gPmMaze[ty][tx];
            if (*t == 2) { *t = 0; gPmScore += 10; gPmDots--; }
            else if (*t == 3) {
                *t = 0; gPmScore += 50; gPmDots--;
                gPmFright = 200; gPmKills = 0;
                for (i = 0; i < 3; i++)
                    if (gPmGh[i].state == GH_SCATTER || gPmGh[i].state == GH_CHASE)
                        { gPmGh[i].state = GH_FRIGHT; gPmGh[i].dir ^= 2; }
            }
            if (!gPmDots) {
                gPmLevel++;
                pm_load_maze();
                pm_reset_actors();
                gPmState = PM_READY;
                gPmStateT = TickCount() + 66;
                return;
            }
            if (pm_walkable(tx + kPmDX[gPmNextDir], ty + kPmDY[gPmNextDir], 0, 0))
                gPmDir = gPmNextDir;
            if (pm_walkable(tx + kPmDX[gPmDir], ty + kPmDY[gPmDir], 0, 0)) {
                gPmX += kPmDX[gPmDir]; gPmY += kPmDY[gPmDir];
            }
        } else {
            gPmX += kPmDX[gPmDir]; gPmY += kPmDY[gPmDir];
        }
        if (gPmX < 0) gPmX = (PM_COLS - 1) * PM_TILE;
        if (gPmX > (PM_COLS - 1) * PM_TILE) gPmX = 0;

        /* --- ghosts --- */
        for (i = 0; i < 3; i++) {
            PmGhost *g = &gPmGh[i];
            if (g->state == GH_HOUSE) {
                if (sub == 0 && --g->timer <= 0) {
                    g->x = 14 * PM_TILE; g->y = 10 * PM_TILE;
                    g->dir = D_LEFT;
                    g->state = pm_mode_state();
                }
                continue;
            }
            if ((g->x % PM_TILE) == 0 && (g->y % PM_TILE) == 0) {
                if (g->state == GH_EATEN &&
                    g->x == 14 * PM_TILE && g->y == 10 * PM_TILE)
                    g->state = pm_mode_state();
                pm_ghost_steer(i);
            }
            /* frightened ghosts move at half speed */
            if (g->state == GH_FRIGHT && sub == 1) continue;
            if (pm_walkable((g->x + kPmDX[g->dir] * PM_TILE) / PM_TILE,
                            (g->y + kPmDY[g->dir] * PM_TILE) / PM_TILE,
                            1, g->state == GH_EATEN) ||
                (g->x % PM_TILE) || (g->y % PM_TILE)) {
                g->x += kPmDX[g->dir]; g->y += kPmDY[g->dir];
            }
            if (g->x < 0) g->x = (PM_COLS - 1) * PM_TILE;
            if (g->x > (PM_COLS - 1) * PM_TILE) g->x = 0;

            /* collision */
            {
                short dx = g->x > gPmX ? g->x - gPmX : gPmX - g->x;
                short dy = g->y > gPmY ? g->y - gPmY : gPmY - g->y;
                if (dx < 6 && dy < 6) {
                    if (g->state == GH_FRIGHT) {
                        g->state = GH_EATEN;
                        gPmScore += 200L << gPmKills;
                        if (gPmKills < 3) gPmKills++;
                    } else if (g->state != GH_EATEN) {
                        pm_kill_pac();
                        return;
                    }
                }
            }
        }
    }
}

static void pm_tile_rect(UnoWin *w, short tx, short ty, Rect *r)
{
    SetRect(r, w->bounds.left + 4 + tx * PM_TILE,
            w->bounds.top + TBAR_H + 2 + ty * PM_TILE,
            w->bounds.left + 4 + tx * PM_TILE + PM_TILE,
            w->bounds.top + TBAR_H + 2 + ty * PM_TILE + PM_TILE);
}

static void pacman_draw(UnoWin *w)
{
    short r, c, i, px;
    Rect q;
    char num[16];

    /* maze backdrop: window content is cleared by draw_window; draw black */
    SetRect(&q, w->bounds.left + 4, w->bounds.top + TBAR_H + 2,
            w->bounds.left + 4 + PM_COLS * PM_TILE,
            w->bounds.top + TBAR_H + 2 + PM_ROWS * PM_TILE);
#if UNO_COLOR
    { RGBColor blk = {0,0,0}; RGBForeColor(&blk); PaintRect(&q); RGBForeColor(&kBlack); }
#else
    FillRect(&q, &qd.black);
#endif

    if (gPmState == PM_TITLE) {
        text_at(w->bounds.left + 4 + 84, w->bounds.top + TBAR_H + 60,
                "P A C - M A N", C_WHITE, C_BLUE, false);
        text_at(w->bounds.left + 4 + 76, w->bounds.top + TBAR_H + 110,
                "N: new game", C_CYAN, C_BLUE, false);
        return;
    }

    for (r = 0; r < PM_ROWS; r++)
        for (c = 0; c < PM_COLS; c++) {
            unsigned char t = gPmMaze[r][c];
            if (t == 1) {
                pm_tile_rect(w, c, r, &q);
                InsetRect(&q, 1, 1);
                uno_fill(&q, C_CYAN);
            } else if (t == 2) {
                pm_tile_rect(w, c, r, &q);
                InsetRect(&q, 3, 3);
                uno_fill(&q, C_WHITE);
            } else if (t == 3) {
                pm_tile_rect(w, c, r, &q);
                InsetRect(&q, 2, 2);
                uno_fill(&q, C_WHITE);
            } else if (t == 5) {
                pm_tile_rect(w, c, r, &q);
                q.top += 3; q.bottom -= 3;
                uno_fill(&q, C_MAG);
            }
        }

    /* pac */
    if (gPmState != PM_DEAD) {
        SetRect(&q, w->bounds.left + 4 + gPmX,
                w->bounds.top + TBAR_H + 2 + gPmY,
                w->bounds.left + 4 + gPmX + 7,
                w->bounds.top + TBAR_H + 2 + gPmY + 7);
#if UNO_COLOR
        RGBForeColor(&kPalette[C_WHITE]); PaintOval(&q); RGBForeColor(&kBlack);
#else
        PaintOval(&q);
#endif
    }
    /* ghosts: Blinky red, Pinky pink, Clyde orange; frightened deep blue */
    for (i = 0; i < 3; i++) {
        static const GameRGB kGhRGB[3] = {
            { 230, 40, 30, C_MAG }, { 250, 150, 200, C_MAG }, { 245, 160, 50, C_MAG },
        };
        static const GameRGB kGhFr  = { 40, 40, 200, C_CYAN };
        static const GameRGB kGhFl  = { 240, 240, 240, C_WHITE };
        PmGhost *g = &gPmGh[i];
        const GameRGB *grgb = &kGhRGB[i];
        short col = C_MAG;
        if (g->state == GH_FRIGHT) {
            grgb = (gPmFright < 70 && (gPmFright & 8)) ? &kGhFl : &kGhFr;
            col = (gPmFright < 70 && (gPmFright & 8)) ? C_MAG : C_CYAN;
        }
        SetRect(&q, w->bounds.left + 4 + g->x,
                w->bounds.top + TBAR_H + 2 + g->y,
                w->bounds.left + 4 + g->x + 7,
                w->bounds.top + TBAR_H + 2 + g->y + 7);
        if (g->state == GH_EATEN) {
            InsetRect(&q, 2, 2);
            uno_fill(&q, C_WHITE);
        } else {
            (void)col;
            fill_rgb(&q, grgb);
            {
                Rect e = q;
                e.right = e.left + 2; e.bottom = e.top + 2;
                OffsetRect(&e, 1, 1); uno_fill(&e, C_WHITE);
                OffsetRect(&e, 3, 0); uno_fill(&e, C_WHITE);
            }
        }
    }

    /* HUD */
    px = w->bounds.left + 4 + PM_COLS * PM_TILE + 8;
    text_at(px, w->bounds.top + TBAR_H + 14, "SCORE", C_CYAN, C_BLUE, false);
    fmt_u(gPmScore, num);
    text_at(px, w->bounds.top + TBAR_H + 28, num, C_WHITE, C_BLUE, false);
    text_at(px, w->bounds.top + TBAR_H + 48, "HI", C_CYAN, C_BLUE, false);
    fmt_u(gPmHi, num);
    text_at(px, w->bounds.top + TBAR_H + 62, num, C_WHITE, C_BLUE, false);
    text_at(px, w->bounds.top + TBAR_H + 82, "LIVES", C_CYAN, C_BLUE, false);
    fmt_u(gPmLives, num);
    text_at(px, w->bounds.top + TBAR_H + 96, num, C_WHITE, C_BLUE, false);
    text_at(px, w->bounds.top + TBAR_H + 116, "LEVEL", C_CYAN, C_BLUE, false);
    fmt_u(gPmLevel, num);
    text_at(px, w->bounds.top + TBAR_H + 130, num, C_WHITE, C_BLUE, false);

    if (gPmState == PM_READY)
        text_at(w->bounds.left + 4 + 88, w->bounds.top + TBAR_H + 108,
                "READY!", C_WHITE, C_BLUE, false);
    else if (gPmState == PM_OVER)
        text_at(w->bounds.left + 4 + 76, w->bounds.top + TBAR_H + 108,
                "GAME  OVER", C_WHITE, C_BLUE, false);
}

static Boolean pacman_key(char ch, short code)
{
    if (ch == 'n' || ch == 'N') { pm_new_game(); return true; }
    if (gPmState != PM_PLAY && gPmState != PM_READY) return false;
    if (code == 0x7E || ch == 0x1E) { gPmNextDir = D_UP;    return true; }
    if (code == 0x7D || ch == 0x1F) { gPmNextDir = D_DOWN;  return true; }
    if (code == 0x7B || ch == 0x1C) { gPmNextDir = D_LEFT;  return true; }
    if (code == 0x7C || ch == 0x1D) { gPmNextDir = D_RIGHT; return true; }
    return false;
}

static void pacman_tick(void)
{
    UnoWin *w;
    long now = TickCount();
    if (gPmState == PM_TITLE || gPmState == PM_OVER) return;
    if (!(gZCount && zwin(gZCount - 1)->proc == APP_PACMAN)) return;
    if (gPmState == PM_READY) {
        if (now >= gPmStateT) gPmState = PM_PLAY;
        else return;
    }
    if (now - gPmLastStep < 2) return;
    gPmLastStep = now;
    pm_step();
    w = find_app_window(APP_PACMAN);
    if (w) draw_window(w);
}

/* =========================================================================
 * Theme app (color targets only) - 8 presets + per-channel custom editing
 * ========================================================================= */
#if UNO_COLOR
static void theme_draw(UnoWin *w)
{
    Rect r = w->bounds, row;
    short x = r.left + 10, y0 = r.top + TBAR_H + 6, i;
    char line[48], num[12];

    for (i = 0; i < NTHEMES; i++) {
        short ry = y0 + i * 16;
        Boolean sel = (i == gTSel);
        SetRect(&row, r.left + 4, ry, r.right - 4, ry + 16);
        if (sel) uno_fill(&row, C_CYAN);
        text_at(x, ry + 12, kThemeNames[i], sel ? C_BLUE : C_WHITE,
                sel ? C_CYAN : C_BLUE, false);
    }
    {
        short cy = y0 + NTHEMES * 16 + 14;
        const RGBColor *c = &kPalette[gTSlot];
        strcpy(line, "Custom  Slot ");
        fmt_u(gTSlot, num); strcat(line, num);
        strcat(line, "   R/G/B  ");
        fmt_u(c->red >> 12, num);   strcat(line, num); strcat(line, "/");
        fmt_u(c->green >> 12, num); strcat(line, num); strcat(line, "/");
        fmt_u(c->blue >> 12, num);  strcat(line, num);
        text_at(x, cy, line, C_WHITE, C_BLUE, false);
        text_at(x, cy + 16, "Enter: apply   r/g/b: tune   </>: slot",
                C_CYAN, C_BLUE, false);
    }
}

static void theme_tune(short chan)
{
    RGBColor *c = &kPalette[gTSlot];
    unsigned short *v = (chan == 0) ? &c->red : (chan == 1) ? &c->green
                                              : &c->blue;
    *v = (unsigned short)((((*v >> 12) + 1) & 15) * 0x1111);
    repaint_all();
}

static Boolean theme_key(char ch, short code)
{
    UnoWin *w = find_app_window(APP_THEME);
    if (code == 0x7D || ch == 0x1F) {                   /* down */
        if (gTSel < NTHEMES - 1) gTSel++;
        if (w) draw_window(w);
        return true;
    }
    if (code == 0x7E || ch == 0x1E) {                   /* up */
        if (gTSel > 0) gTSel--;
        if (w) draw_window(w);
        return true;
    }
    if (code == 0x7B || ch == 0x1C) {                   /* left: prev slot */
        gTSlot = (gTSlot + 3) & 3;
        if (w) draw_window(w);
        return true;
    }
    if (code == 0x7C || ch == 0x1D) {                   /* right: next slot */
        gTSlot = (gTSlot + 1) & 3;
        if (w) draw_window(w);
        return true;
    }
    if (ch == 0x0D || ch == 0x03) {                     /* apply preset */
        memcpy(kPalette, kThemes[gTSel], sizeof(kPalette));
        repaint_all();
        return true;
    }
    if (ch == 'r' || ch == 'R') { theme_tune(0); return true; }
    if (ch == 'g' || ch == 'G') { theme_tune(1); return true; }
    if (ch == 'b' || ch == 'B') { theme_tune(2); return true; }
    return false;
}
#endif /* UNO_COLOR */

/* =========================================================================
 * Desktop
 * ========================================================================= */
static void icon_xy(short i, short *x, short *y)
{
    *x = (short)(ICON0_X + (i % ICONS_ROW) * ICON_PITCH);
    *y = (short)(ICON0_Y + (i / ICONS_ROW) * ICON_ROW_H);
}

static void icon_rect(short i, Rect *r)
{
    short x, y;
    icon_xy(i, &x, &y);
    SetRect(r, x - 4, y - 4, x + 24, y + 24);
}

static void draw_icon(short i)
{
    Rect cell, g;
    short x, iy;
    icon_xy(i, &x, &iy);
    icon_rect(i, &cell);
    desktop_bg(&cell);
    switch (i) {
    case APP_SYSINFO:                       /* monitor */
        SetRect(&g, x, iy, x + 18, iy + 13);
        uno_box(&g, C_CYAN);
        { Rect inr = g; InsetRect(&inr, 2, 2); uno_fill(&inr, C_CYAN); }
        SetRect(&g, x + 6, iy + 13, x + 12, iy + 16); uno_box(&g, C_CYAN);
        break;
    case APP_CLOCK:                         /* clock face */
        SetRect(&g, x, iy, x + 16, iy + 16);
#if UNO_COLOR
        RGBForeColor(&kPalette[C_CYAN]);
#else
        ForeColor(blackColor);
#endif
        FrameOval(&g);
        MoveTo(x + 8, iy + 8); LineTo(x + 8, iy + 3);
        MoveTo(x + 8, iy + 8); LineTo(x + 12, iy + 8);
#if UNO_COLOR
        RGBForeColor(&kBlack);
#endif
        break;
    case APP_FILES:                         /* folder */
        SetRect(&g, x, iy + 3, x + 18, iy + 15);
        uno_fill(&g, C_CYAN);
        SetRect(&g, x, iy, x + 9, iy + 5);
        uno_fill(&g, C_CYAN);
        SetRect(&g, x, iy + 3, x + 18, iy + 15);
        uno_box(&g, C_WHITE);
        break;
    case APP_NOTEPAD:                       /* page with lines */
        SetRect(&g, x + 1, iy, x + 15, iy + 17);
        uno_fill(&g, C_WHITE);
        uno_box(&g, C_CYAN);
        {
            short ly;
            for (ly = iy + 3; ly < iy + 15; ly += 3) {
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
    case APP_DOSTRIS:                       /* stacked blocks */
        SetRect(&g, x, iy + 10, x + 8, iy + 17);  uno_fill(&g, C_CYAN);
        SetRect(&g, x + 9, iy + 10, x + 17, iy + 17); uno_fill(&g, C_MAG);
        SetRect(&g, x + 5, iy + 2, x + 13, iy + 9); uno_fill(&g, C_WHITE);
        break;
    case APP_OUTLAST:                       /* road to the horizon */
        SetRect(&g, x, iy, x + 18, iy + 17);
        uno_fill(&g, C_CYAN);
        { Rect rd;
          SetRect(&rd, x + 7, iy, x + 11, iy + 17); uno_fill(&rd, C_WHITE);
          SetRect(&rd, x + 8, iy + 12, x + 10, iy + 17); uno_fill(&rd, C_MAG); }
        break;
    case APP_PACMAN:                        /* pac chasing a dot */
        SetRect(&g, x, iy + 2, x + 13, iy + 15);
#if UNO_COLOR
        RGBForeColor(&kPalette[C_WHITE]); PaintOval(&g); RGBForeColor(&kBlack);
#else
        PaintOval(&g);
#endif
        SetRect(&g, x + 15, iy + 7, x + 18, iy + 10);
        uno_fill(&g, C_MAG);
        break;
#if UNO_COLOR
    case APP_THEME:                         /* palette swatches */
        SetRect(&g, x, iy, x + 8, iy + 8);      uno_fill(&g, C_CYAN);
        SetRect(&g, x + 9, iy, x + 17, iy + 8); uno_fill(&g, C_MAG);
        SetRect(&g, x, iy + 9, x + 8, iy + 17); uno_fill(&g, C_WHITE);
        SetRect(&g, x + 9, iy + 9, x + 17, iy + 17); uno_box(&g, C_WHITE);
        break;
#endif
    case APP_TRACKER: {                     /* pattern grid */
        short c2;
        SetRect(&g, x, iy, x + 18, iy + 17); uno_box(&g, C_CYAN);
        for (c2 = 0; c2 < 3; c2++) {
            SetRect(&g, (short)(x + 3 + c2 * 5), (short)(iy + 3 + c2 * 3),
                        (short)(x + 6 + c2 * 5), (short)(iy + 14));
            uno_fill(&g, (short)(c2 == 1 ? C_MAG : C_WHITE));
        }
        break;
    }
    case APP_PAINT:                         /* brush + daub */
        SetRect(&g, x + 2, iy, x + 7, iy + 9); uno_fill(&g, C_CYAN);
        SetRect(&g, x + 3, iy + 9, x + 6, iy + 13); uno_fill(&g, C_WHITE);
        SetRect(&g, x + 9, iy + 11, x + 17, iy + 17); uno_fill(&g, C_MAG);
        break;
    case APP_MUSIC:                         /* eighth note */
        SetRect(&g, x + 2, iy + 11, x + 8, iy + 17);
        uno_fill(&g, C_CYAN);
#if UNO_COLOR
        RGBForeColor(&kPalette[C_CYAN]);
#else
        ForeColor(blackColor);
#endif
        MoveTo(x + 7, iy + 13); LineTo(x + 7, iy);
        LineTo(x + 14, iy + 3);
#if UNO_COLOR
        RGBForeColor(&kBlack);
#endif
        break;
    }
    text_at(x - 8, iy + 28, kIconNames[i], C_WHITE, C_BLUE, true);
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
    /* focused window gets the key via its task mailbox (milestone 3);
       ESC stays kernel-side, like the asm ports */
    if (gZCount > 0) {
        if (ch == 0x1B) { close_window(gZCount - 1); return; }  /* ESC */
        task_post_key(gZ[gZCount - 1], (short)(unsigned char)ch, code, cmd);
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
 * Splash - "UnoDOS 3" + a happy compact Mac, ~2s (per-platform splash
 * identity). Color: white art on the desktop blue; mono: black on white.
 * ========================================================================= */
static void splash_ink(short fg)
{
#if UNO_COLOR
    RGBForeColor(&kPalette[fg]);
#else
    (void)fg;
    ForeColor(blackColor);
#endif
}

static void splash_show(void)
{
    short cx = (short)((gScreen.left + gScreen.right) / 2);
    short cy = (short)((gScreen.top + gScreen.bottom) / 2) - 40;
    Rect r;
    long until;
    const char *title = "UnoDOS 3";
    const char *sub = "for Apple Macintosh";

#if UNO_COLOR
    desktop_bg(&gScreen);
#else
    FillRect(&gScreen, &qd.white);
#endif

    /* compact Mac: case, screen, happy face, floppy slot, foot */
    splash_ink(C_WHITE);
    PenSize(2, 2);
    SetRect(&r, cx - 34, cy - 48, cx + 34, cy + 34);
    FrameRoundRect(&r, 10, 10);
    SetRect(&r, cx - 24, cy - 38, cx + 24, cy - 2);
    FrameRect(&r);
    PenSize(1, 1);
    /* eyes */
    MoveTo(cx - 10, cy - 30); LineTo(cx - 10, cy - 24);
    MoveTo(cx + 10, cy - 30); LineTo(cx + 10, cy - 24);
    /* nose */
    MoveTo(cx, cy - 24); LineTo(cx, cy - 19); LineTo(cx + 3, cy - 19);
    /* smile */
    SetRect(&r, cx - 12, cy - 26, cx + 12, cy - 10);
    FrameArc(&r, 135, 90);
    /* floppy slot */
    MoveTo(cx + 6, cy + 14); LineTo(cx + 24, cy + 14);
    /* foot */
    SetRect(&r, cx - 26, cy + 34, cx + 26, cy + 42);
    splash_ink(C_CYAN);
    PaintRect(&r);

    /* title */
    TextFont(0);
    TextFace(bold);
    TextSize(36);
    splash_ink(C_WHITE);
    MoveTo((short)(cx - TextWidth((Ptr)title, 0, (short)strlen(title)) / 2),
           (short)(cy + 90));
    DrawText((Ptr)title, 0, (short)strlen(title));
    TextSize(12);
    TextFace(0);
    splash_ink(C_CYAN);
    MoveTo((short)(cx - TextWidth((Ptr)sub, 0, (short)strlen(sub)) / 2),
           (short)(cy + 112));
    DrawText((Ptr)sub, 0, (short)strlen(sub));
#if UNO_COLOR
    RGBForeColor(&kBlack);
#else
    ForeColor(blackColor);
#endif

#ifdef UNO_AUTOTEST_SPLASH
    until = TickCount() + 60000L;       /* long hold for screenshot runs */
#else
    until = TickCount() + 120;          /* ~2s */
#endif
    /* pump the event loop while holding: the system (and Executor) only
       flushes the screen and runs SystemTask inside GetNextEvent */
    while (TickCount() < until) {
        EventRecord ev;
        GetNextEvent(everyEvent, &ev);
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
#ifdef UNO_EE
    uno_ee_init();                  /* GS + pad up before any drawing */
#endif
    sched_init();                   /* milestone 3: cooperative tasks */
    gScreen = qd.screenBits.bounds;

    r = gScreen;
#if UNO_COLOR
    gWin = NewCWindow(NULL, &r, " ", true, plainDBox, (WindowPtr)-1L, false, 0);
#else
    gWin = NewWindow(NULL, &r, " ", true, plainDBox, (WindowPtr)-1L, false, 0);
#endif
    SetPort(gWin);
    TextFont(0);
    TextSize(12);

    gBootTicks = TickCount();
    gSel = 0;
    splash_show();
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
#if defined(UNO_AUTOTEST_THEME) && UNO_COLOR
    /* Theme variant: open the picker, select Sunset, apply through the
       real key handler. */
    launch_app(APP_THEME);
    theme_key(0, 0x7D); theme_key(0, 0x7D); theme_key(0, 0x7D);
    theme_key(0x0D, 0);
#endif
#ifdef UNO_AUTOTEST_DOSTRIS
    /* Dostris variant: start a game and hard-drop eight pieces through the
       real key handler so the screenshot shows a played board. */
    launch_app(APP_DOSTRIS);
    dostris_key('n', 0);
    {
        short i;
        for (i = 0; i < 8; i++) {
            dostris_key(0, 0x7B);           /* nudge left */
            dostris_key(' ', 0);            /* hard drop */
        }
    }
#endif
#ifdef UNO_AUTOTEST_OUTLAST
    /* OutLast variant: start driving and run 80 physics steps so the
       screenshot shows the road mid-game. */
    launch_app(APP_OUTLAST);
    outlast_key('n', 0);
    {
        short i;
        for (i = 0; i < 80; i++) {
            gOlLastStep = -100;             /* force a step regardless of ticks */
            outlast_tick();
        }
    }
#endif
#ifdef UNO_AUTOTEST_PACMAN
    /* Pac-Man variant: start a game and run 150 steps so the screenshot
       shows the maze mid-game with ghosts loose. */
    launch_app(APP_PACMAN);
    pacman_key('n', 0);
    gPmState = PM_PLAY;
    {
        short i;
        for (i = 0; i < 150; i++) pm_step();
    }
#endif
#ifdef UNO_AUTOTEST_TRACKER
    /* Tracker: demo song, cursor down 5 rows, playback on */
    launch_app(APP_TRACKER);
    tracker_key('d', 0);
    { short i; for (i = 0; i < 5; i++) tracker_key(0, 0x7D); }
    tracker_key(' ', 0);
#endif
#ifdef UNO_AUTOTEST_PAINT
    /* Paint: draw a scene through the real tool primitives */
    launch_app(APP_PAINT);
    {
        UnoWin *w = find_app_window(APP_PAINT);
        if (w && gPtCanvas) {
#if UNO_COLOR
            gPtColor = 180;                         /* red */
            pt_rect_shape(w, 30, 30, 150, 110, true);
            gPtColor = 35;                          /* green */
            pt_oval_shape(w, 180, 50, 300, 150, true);
            gPtColor = 5;                           /* blue */
            pt_line(w, 20, 200, 380, 140, 1);
            gPtColor = 215;                         /* yellow-ish cube corner */
            pt_rect_shape(w, 230, 170, 330, 220, false);
            gPtColor = 0;
            pt_line(w, 30, 30, 150, 110, 1);
#else
            gPtPat = 1;
            pt_rect_shape(w, 30, 30, 150, 110, false);
            gPtPat = 3;                             /* 50% gray */
            pt_oval_shape(w, 180, 50, 300, 150, true);
            gPtPat = 1;
            pt_line(w, 20, 200, 380, 140, 1);
#endif
            draw_window(w);
        }
    }
#endif
#ifdef UNO_AUTOTEST_FAT12
    /* FAT12 round trip on the RAM volume: format+mount, write README.TXT
       through the core, switch Files to the PC volume, reopen it into
       Notepad - the listing + restored text on screen prove the chain. */
    launch_app(APP_FILES);
    files_key('v', 0);                          /* mount + switch volume */
    {
        const char *demo = "HELLO FROM FAT12\rwritten and read back\rby the portable core.";
        fat12_write("README.TXT", (const unsigned char *)demo, (long)strlen(demo));
        fat12_list();
    }
    files_key('r', 0);
    files_key(0x0D, 0);                         /* Enter: open into Notepad */
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

#ifdef UNO_HOST
    /* host shim: the AUTOTEST blocks above have drawn the desktop + apps into
       the framebuffer. Settle a few frames (animations/clock), present to a
       PPM, and exit - the inner loop for verifying app rendering on the PC. */
    {
        int _i;
        for (_i = 0; _i < 8; _i++) {
            music_tick(); gm_tick(); tracker_tick();
            post_ticks(); task_yield(); app_secondly();
        }
        repaint_all();
        uno_host_present();
        return 0;
    }
#endif

    for (;;) {
        Boolean got;
#ifdef UNO_EE
        uno_ee_poll();              /* DualShock 2 -> event queue */
#endif
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
        gm_tick();
        tracker_tick();
        post_ticks();               /* frame tick -> the focused app task */
        task_yield();               /* run the app tasks (milestone 3) */
        app_secondly();
#ifdef UNO_EE
        uno_ee_present();           /* blit the software fb to the GS */
#endif
    }
    return 0;
}

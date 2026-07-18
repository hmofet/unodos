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
#include <Memory.h>
#include "uno_app.h"   /* shared app ABI: KernelApi/AppInterface/UnoWin/... */
#if defined(UNO_HOST)
#include <stdio.h>
#include <stdlib.h>
#endif
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
/* pointer-based dispatch supplied by app_loader.c (no app code here) */
static const AppInterface *app_iface(short proc);
static const char *app_title(short proc);
static void app_default_rect(short proc, Rect *r);
/* fill_rgb + gm_start are defined after the #included app_loader.c;
   forward-declare them so the loader's KernelApi build compiles. */
static void fill_rgb(Rect *q, const GameRGB *c);
static void gm_start(const Note *notes, short count, short owner);

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
    gWins[slot].title = app_title(proc);
    app_default_rect(proc, &gWins[slot].bounds);
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
    unsigned short v;
    if (off + 1 >= (long)gFatFatSz * 512) return 0xFFF;  /* off the cached FAT -> EOC */
    v = (unsigned short)(gFatCache[off] | (gFatCache[off + 1] << 8));
    return (cl & 1) ? (unsigned short)(v >> 4) : (unsigned short)(v & 0x0FFF);
}

static void fat_set(unsigned short cl, unsigned short val)
{
    long off = (long)cl + ((long)cl >> 1);
    if (off + 1 >= (long)gFatFatSz * 512) return;        /* off the cached FAT -> ignore */
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
 * Music app - Canon in D on the Sound Manager square-wave synth
 * (same arrangement as the x86 apps/music.asm)
 * ========================================================================= */

static SndChannelPtr gSnd = NULL;

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


/* KernelApi-level music control: the Music MODULE owns the song
   sequencer; these are channel-level (open / quiet) for the ABI. */
static void music_start(void) { music_open_chan(); }

static void music_stop(void)  { music_quiet(); }







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

static void app_tick_dispatch(short proc)
{
    const AppInterface *ai = app_iface(proc);
    if (ai && ai->tick) ai->tick();
}

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

/* =========================================================================
 * App dispatch
 * ========================================================================= */
/* App dispatch is now pointer-based: app_loader.c provides
   draw_app_content / app_key / app_click / app_opened / app_close /
   app_title / app_default_rect, dispatching through each module's
   AppInterface.  No switch(proc) on app identity remains in the core. */
#include "app_loader.c"

/* Per-frame tick for every open app window, through the module's tick
   pointer (replaces the old music_tick()/tracker_tick() direct calls). */
static void tick_all_apps(void)
{
    short z;
    /* skip the topmost window (z = gZCount-1): post_ticks() dispatches its tick
       via the app task, so ticking it here too runs its tick() twice per frame
       (AUDIT-mac §1 P2). Ticks are TickCount-gated so this is only waste; drop
       it. Non-topmost windows still tick here. */
    for (z = 0; z + 1 < gZCount; z++)
        app_tick_dispatch(zwin(z)->proc);
}


/* =========================================================================
 * True-color helpers - the color targets run 8-bit Color QuickDraw, so the
 * games use real RGB (nearest of the 256 system colors); the mono target
 * maps each entry to a 4-color theme slot for the 1-bit look.
 * ========================================================================= */

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
    sched_init();                   /* milestone 3: cooperative tasks */
    app_loader_init();              /* build the KernelApi for loaded modules */
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
    splash_show();
    draw_desktop();

#ifdef UNO_AUTOTEST_FILES
    launch_app(APP_FILES);
#endif
#if defined(UNO_AUTOTEST_THEME) && UNO_COLOR
    launch_app(APP_THEME);
#endif
#ifdef UNO_AUTOTEST_DOSTRIS
    launch_app(APP_DOSTRIS);
#endif
#ifdef UNO_AUTOTEST_OUTLAST
    launch_app(APP_OUTLAST);
#endif
#ifdef UNO_AUTOTEST_PACMAN
    launch_app(APP_PACMAN);
#endif
#ifdef UNO_AUTOTEST_TRACKER
    launch_app(APP_TRACKER);
#endif
#ifdef UNO_AUTOTEST_PAINT
    launch_app(APP_PAINT);
#endif
#ifdef UNO_AUTOTEST_FAT12
    launch_app(APP_FILES);
#endif
#ifdef UNO_AUTOTEST
    launch_app(APP_MUSIC);
    launch_app(APP_FILES);
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
        gm_tick();
        tick_all_apps();
        post_ticks();               /* frame tick -> the focused app task */
        task_yield();               /* run the app tasks (milestone 3) */
        app_secondly();
    }
    return 0;
}

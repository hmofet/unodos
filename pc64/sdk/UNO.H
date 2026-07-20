/* ===========================================================================
 * UNO.H - the UnoDOS pc64 application SDK, for apps built WITH UnoDOS
 * (the Studio IDE + its built-in UnoC compiler).
 *
 * This is the whole world an app sees: the shared-ABI types (byte-identical
 * to the kernel's uno_app.h / mac_compat.h), the KernelApi callback table
 * handed to the entry point, and the kernel exports an app may call
 * directly.  A UnoC app defines exactly one entry:
 *
 *     const AppInterface *uno_app_main(const KernelApi *k) {
 *         gK = k;
 *         return &kIface;
 *     }
 *
 * and draws/reacts through the AppInterface vtable it returns.  See
 * DOCS\APPDEV.MD for the app model and DOCS\LANG.MD for the UnoC subset.
 * (PtInRect is not in the SDK: it takes a Point by value, which UnoC
 * does not do - compare p.h/p.v against the rect yourself.)
 * ======================================================================== */
#ifndef UNO_H
#define UNO_H

typedef unsigned char Boolean;
#define true  1
#define false 0

typedef char *Ptr;
typedef struct { short v, h; } Point;
typedef struct { short top, left, bottom, right; } Rect;
typedef struct { unsigned short red, green, blue; } RGBColor;

typedef struct {
    Boolean     used;
    short       proc;
    Rect        bounds;
    const char *title;
} UnoWin;

/* app/window proc ids (find_app_window / launch_app / gm_start owner) */
enum { APP_SYSINFO = 0, APP_CLOCK, APP_FILES, APP_NOTEPAD, APP_MUSIC,
       APP_DOSTRIS, APP_OUTLAST, APP_PACMAN, APP_TRACKER, APP_PAINT,
       APP_THEME, APP_SETTINGS, APP_NETWORK, APP_RUNNER, APP_NAPPS };

/* the desktop's 4-slot window palette */
enum { C_BLUE = 0, C_CYAN = 1, C_MAG = 2, C_WHITE = 3 };

typedef struct { unsigned char midi; unsigned char dur; } Note;
typedef struct { const Note *notes; short count; const char *title; } Song;
typedef struct { unsigned char r, g, b, mono; } GameRGB;

/* ---- the kernel callback table (layout = uno_app.h, do not reorder) ------- */
typedef struct KernelApi {
    short  abi_version;
    RGBColor *palette;
    RGBColor *black;
    short  tbar_h;

    void (*uno_fill)(Rect *r, short c);
    void (*uno_box)(Rect *r, short c);
    void (*uno_invert)(Rect *r);
    void (*text_at)(short x, short y, const char *s, short fg, short bg, Boolean opaque);
    void (*text_at_max)(short x, short y, const char *s, short fg, short maxw);
    void (*fill_rgb)(Rect *q, const GameRGB *c);

    void (*fmt_u)(long v, char *out);
    void (*put2)(long v, char *out);

    long (*now_secs)(void);

    void    (*draw_window)(UnoWin *w);
    UnoWin *(*find_app_window)(short proc);
    void    (*launch_app)(short proc);
    void    (*repaint_all)(void);
    short   (*topmost_proc)(void);

    Boolean (*fat12_mount)(void);
    void    (*fat12_list)(void);
    long    (*fat12_read)(const char *name, unsigned char *buf, long max);
    Boolean (*fat12_write)(const char *name, const unsigned char *buf, long len);
    short  *fat_count;
    unsigned char (*fat_name)[13];
    long   *fat_sizes;

    void (*music_open_chan)(void);
    void (*music_note_on)(short midi, short durTicks);
    void (*music_quiet)(void);
    void (*music_start)(void);
    void (*music_stop)(void);
    void (*gm_start)(const Note *notes, short count, short owner);
    void (*gm_stop)(void);

    int  (*display_res_count)(void);
    void (*display_res_get)(int idx, short *w, short *h, short *zoom,
                            Boolean *active);
    void (*display_res_set)(int idx);
} KernelApi;

/* ---- the vtable an app returns -------------------------------------------- */
typedef struct AppInterface {
    void    (*draw)(UnoWin *w);
    Boolean (*key)(char ch, short code, Boolean cmd);
    void    (*click)(UnoWin *w, Point p);
    void    (*tick)(void);
    void    (*opened)(void);
    void    (*closed)(void);
    const char *win_title;
    short   win_rect[4];
} AppInterface;

/* ---- kernel exports an app may call by name ------------------------------- */
/* Toolbox geometry + drawing */
void SetRect(Rect *r, short left, short top, short right, short bottom);
void OffsetRect(Rect *r, short dh, short dv);
void InsetRect(Rect *r, short dh, short dv);
void PaintRect(const Rect *r);
void FrameRect(const Rect *r);
void InvertRect(const Rect *r);
void PaintOval(const Rect *r);
void FrameOval(const Rect *r);
void MoveTo(short h, short v);
void LineTo(short h, short v);
void PenNormal(void);
void PenMode(short mode);
void RGBForeColor(const RGBColor *c);
/* events / time / memory */
long TickCount(void);
void GetMouse(Point *mouseLoc);
Boolean StillDown(void);
short Random(void);
Ptr  NewPtr(long byteCount);
void DisposePtr(Ptr p);
/* libc */
void *memcpy(void *d, const void *s, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
int   memcmp(const void *a, const void *b, unsigned long n);
unsigned long strlen(const char *s);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, unsigned long n);
char *strcat(char *d, const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, unsigned long n);

/* ---- the per-app prelude (same shape as the shipped apps' uno_mod.h) ------ */
static const KernelApi *gK;

#define kPalette        (gK->palette)
#define kBlack          (*gK->black)
#define TBAR_H          (gK->tbar_h)
#define uno_fill        gK->uno_fill
#define uno_box         gK->uno_box
#define uno_invert      gK->uno_invert
#define text_at         gK->text_at
#define text_at_max     gK->text_at_max
#define fill_rgb        gK->fill_rgb
#define fmt_u           gK->fmt_u
#define put2            gK->put2
#define now_secs        gK->now_secs
#define draw_window     gK->draw_window
#define find_app_window gK->find_app_window
#define launch_app      gK->launch_app
#define repaint_all     gK->repaint_all
#define fat12_mount     gK->fat12_mount
#define fat12_list      gK->fat12_list
#define fat12_read      gK->fat12_read
#define fat12_write     gK->fat12_write
#define gFatCount       (*gK->fat_count)
#define gFatNames       (gK->fat_name)
#define gFatSizes       (gK->fat_sizes)
#define music_open_chan gK->music_open_chan
#define music_note_on   gK->music_note_on
#define music_quiet     gK->music_quiet
#define music_start     gK->music_start
#define music_stop      gK->music_stop
#define gm_start        gK->gm_start
#define gm_stop         gK->gm_stop

#endif /* UNO_H */

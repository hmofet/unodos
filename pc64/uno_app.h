/* ===========================================================================
 * UnoDOS shared app ABI  -  the C analogue of the C64 port's kernel_api.inc.
 * ===========================================================================
 * The kernel (unodos.c) contains NO app code.  Each of the 11 apps is a
 * separate translation unit (apps/<name>.c) compiled into its own loadable
 * MODULE and loaded from storage at runtime.  An app exports exactly one entry:
 *
 *     const AppInterface *uno_app_main(const KernelApi *k);
 *
 * given the kernel's callback table it stashes the pointer and returns its
 * AppInterface (a vtable of draw/key/click/tick/opened/closed).  The kernel's
 * generic loader resolves the entry, calls it, stores the returned
 * AppInterface in a per-window slot, and the window manager dispatches purely
 * through the pointers - no switch(proc) on app identity in the kernel.
 *
 * Shared verbatim by the kernel and every app module so the ABI is identical
 * on both sides of the load boundary.  Platform-neutral: pulls in whichever
 * Toolbox surface the port uses (real <Quickdraw.h>... on Retro68/Mac, or
 * mac_compat.h on PS2/DC/host).
 * ===========================================================================
 */
#ifndef UNO_APP_H
#define UNO_APP_H

#if defined(UNO_EE) || defined(UNO_DC) || defined(UNO_HOST) || defined(UNO_PC64)
#  include "mac_compat.h"
#else
#  include <Quickdraw.h>
#  include <Windows.h>
#  include <OSUtils.h>
#endif

typedef struct UnoWin {
    Boolean     used;
    short       proc;
    Rect        bounds;
    const char *title;
} UnoWin;

enum { APP_SYSINFO = 0, APP_CLOCK, APP_FILES, APP_NOTEPAD, APP_MUSIC,
       APP_DOSTRIS, APP_OUTLAST, APP_PACMAN, APP_TRACKER, APP_PAINT,
       APP_THEME, APP_SETTINGS, APP_NETWORK, APP_RUNNER, APP_NAPPS };

typedef struct { unsigned char midi; unsigned char dur; } Note;
typedef struct { const Note *notes; short count; const char *title; } Song;
typedef struct { unsigned char r, g, b, mono; } GameRGB;

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

    /* ---- pc64 additive tail (display control; NULL on other ports) ------ */
    int  (*display_res_count)(void);
    void (*display_res_get)(int idx, short *w, short *h, short *zoom,
                            Boolean *active);
    void (*display_res_set)(int idx);
} KernelApi;

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

typedef const AppInterface *(*UnoAppEntry)(const KernelApi *k);

#define UNO_APP_ENTRY_NAME "uno_app_main"
#define UNO_ABI_VERSION    1

#endif /* UNO_APP_H */

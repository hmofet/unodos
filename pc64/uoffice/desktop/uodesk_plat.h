/* ===========================================================================
 * uodesk_plat.h - the one seam between the desktop shell and an OS.
 *
 * Everything ABOVE this line is ours and identical on every OS: the
 * framebuffer (pc64/fb.c), unoui, the Office chrome and the apps.
 * Everything BELOW it is that OS's own API, used directly:
 *
 *   plat_win32.c   Win32 (user32/gdi32/comdlg32)     Windows
 *   plat_cocoa.m   AppKit                            macOS
 *   plat_x11.c     Xlib + the desktop portal (D-Bus) Linux / BSD
 *
 * A backend does four jobs and nothing else: it makes one top-level window
 * and blits our finished framebuffer into it (no drawing of its own), turns
 * the OS's keyboard and mouse input into plat_events, provides the desktop
 * services (title, fullscreen, the file picker, where our files are
 * installed), and keeps time.  No third-party library sits in between.
 * ======================================================================== */
#ifndef UODESK_PLAT_H
#define UODESK_PLAT_H

#include "fb.h"

enum {                      /* plat_event.type                               */
    PE_NONE = 0,
    PE_QUIT,                /* the window's close box, Alt+F4, Cmd+Q         */
    PE_RESIZE,              /* .w .h: the new client size, in our pixels     */
    PE_EXPOSE,              /* the OS lost our pixels: present again         */
    PE_KEY,                 /* .key (PK_*), .ch for PK_CHAR, .mods          */
    PE_TEXT,                /* .text: typed text, UTF-8, NUL-terminated     */
    PE_MOUSE_MOVE,          /* .x .y                                         */
    PE_MOUSE_DOWN,          /* .x .y .button (0 left, 1 right, 2 middle)     */
    PE_MOUSE_UP,
    PE_WHEEL                /* .wheel: notches, + = towards the user (down)  */
};

enum {                      /* plat_event.key                                */
    PK_NONE = 0,
    PK_UP, PK_DOWN, PK_RIGHT, PK_LEFT, PK_HOME, PK_END, PK_INSERT, PK_DELETE,
    PK_PGUP, PK_PGDN, PK_ESC, PK_ENTER, PK_BACKSPACE, PK_TAB,
    PK_F1,                  /* PK_F1 + n for F1..F12                         */
    PK_CHAR = PK_F1 + 12    /* a character key pressed WITH an accelerator
                               modifier (Ctrl, Cmd): .ch is its unshifted,
                               lower-case ASCII - how "Ctrl+S" arrives, since
                               such a press produces no PE_TEXT             */
};

enum { PM_SHIFT = 1, PM_CTRL = 2, PM_ALT = 4, PM_GUI = 8 };   /* == UI_MOD_* */

typedef struct {
    int  type;
    int  key, ch, mods;
    int  x, y, button, wheel;
    int  w, h;
    char text[16];
} plat_event;

/* Create the window (`hidden`: never shown - the headless checks).  0 on
 * failure, after printing why. */
int  plat_init(const char *title, int w, int h, int hidden);
void plat_shutdown(void);

/* Next event, waiting up to `ms` for one; 0 = none arrived. */
int  plat_wait(plat_event *e, int ms);

/* Show a finished frame: `w` x `h` pixels, row stride `w`, fb_px layout
 * (R,G,B,A bytes in memory).  The backend keeps showing it on expose. */
void plat_present(const fb_px *px, int w, int h);

unsigned plat_ticks(void);                  /* milliseconds, monotonic       */
const char *plat_base_path(void);           /* our installed files, with a
                                               trailing separator            */
void plat_set_title(const char *utf8);
void plat_set_fullscreen(int on);
int  plat_is_fullscreen(void);

/* The OS's own Open / Save As dialog, modal over our window.  `types` are
 * uofile's "Name (*.ext;*.ext2)" strings.  Returns 1 with the full UTF-8
 * path in `path` (and the index of the type chosen, or matched from the
 * extension, in *type); 0 if cancelled; -1 if this system has no picker
 * (the caller then draws its own). */
int  plat_file_dialog(int save, const char *const *types, int ntypes,
                      char *path, int cap, int *type);

/* helpers the backends share (uodesk_plat_common.c) */
int  plat_type_patterns(const char *type, char pats[][16], int maxp);
int  plat_type_of_path(const char *path, const char *const *types, int ntypes);
void plat_rgba_to_bgrx(unsigned char *dst, const fb_px *src, int npix);

#endif

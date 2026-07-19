/* ===========================================================================
 * unoui theme model - the swappable look-and-feel vtable.
 *
 * A theme is three things:
 *   1. unoui_palette  - the semantic colours (NOT raw pixels: roles like
 *                       "title bar", "button face", "bevel highlight"). Change
 *                       these and every widget recolours. This is palette
 *                       theming.
 *   2. unoui_metrics  - sizes (title height, bevel thickness, corner radius,
 *                       drop shadow) and the target colour DEPTH.
 *   3. unoui_draw     - a vtable of chrome painters. Leave any entry NULL and
 *                       the portable default is used; override one and that
 *                       widget gets entirely custom GRAPHICS. This is graphics
 *                       theming - it's how Mac Plus draws racing-stripe title
 *                       bars and Windows 3.1 draws double-bevelled buttons.
 *
 * Same contract as uno3d's backend vtable: adding a theme = one new file
 * defining `const unoui_theme theme_<name>`, no core or app edits.
 * ===========================================================================
 */
#ifndef UNOUI_THEME_H
#define UNOUI_THEME_H

#include "unoui.h"

/* target colour depth - drives ui_shade()'s dither-vs-blend decision */
typedef enum {
    UNOUI_DEPTH_FULL = 0,   /* 24-bit truecolour: shades blend smoothly       */
    UNOUI_DEPTH_8,          /* 256-colour: light quantisation                 */
    UNOUI_DEPTH_4,          /* 16-colour EGA/VGA-ish: snap + light dither      */
    UNOUI_DEPTH_1           /* 1-bit B&W: every shade becomes a stipple        */
} unoui_depth;

/* Semantic colour roles. A widget never names a literal colour - it asks the
 * palette for a role, so re-skinning is just swapping this struct. */
typedef struct {
    fb_px desktop;        /* behind all windows                              */
    fb_px desktop2;       /* secondary desktop tone (stipple/pattern partner)*/
    fb_px win_bg;         /* window content background                       */
    fb_px win_frame;      /* outermost window border                         */
    fb_px title_bg;       /* active title bar fill                           */
    fb_px title_fg;       /* active title text                               */
    fb_px title_bg_in;    /* inactive title bar fill                         */
    fb_px title_fg_in;    /* inactive title text                             */
    fb_px text;           /* primary content text                            */
    fb_px text_dim;       /* disabled / secondary text                       */
    fb_px face;           /* control face (buttons, scrollbar thumb)         */
    fb_px face_text;      /* text on a control face                          */
    fb_px light;          /* bevel highlight (top-left of a raised control)  */
    fb_px shadow;         /* bevel shadow   (bottom-right)                   */
    fb_px dark;           /* deepest outline / sunken edge                   */
    fb_px accent;         /* selection / focus / progress fill               */
    fb_px accent_text;    /* text drawn on the accent colour                 */
    fb_px field_bg;       /* text-field / list interior                      */
    fb_px field_text;     /* text-field / list text                          */
} unoui_palette;

typedef struct {
    int title_h;        /* title bar height in px                            */
    int frame_w;        /* window outer frame thickness                      */
    int bevel;          /* 3D control bevel thickness (0 = flat outline)     */
    int pad;            /* default inner content padding                     */
    int radius;         /* corner rounding (0 = square corners)              */
    int closebox;       /* close box size in px (0 = none)                   */
    int shadow_off;     /* window drop-shadow offset (0 = none)              */
    int title_center;   /* 1 = centre title text, 0 = left-align             */
    unoui_depth depth;  /* target colour depth                               */
} unoui_metrics;

struct unoui_theme;     /* fwd for the vtable signatures */

/* Chrome painters. `r` is in absolute screen coords (the core has already
 * translated widget rects through the window's content origin). Any field may
 * be NULL -> the matching unoui_default_* is used. */
typedef struct unoui_draw {
    void (*desktop) (const struct unoui_theme *, int W, int H);
    void (*window)  (const struct unoui_theme *, unoui_window *);          /* frame+bg(+shadow); sets content_x/y */
    void (*titlebar)(const struct unoui_theme *, const unoui_window *);
    void (*button)  (const struct unoui_theme *, unoui_rect, const char *, int flags);
    void (*check)   (const struct unoui_theme *, unoui_rect, const char *, int flags);
    void (*radio)   (const struct unoui_theme *, unoui_rect, const char *, int flags);
    void (*field)   (const struct unoui_theme *, unoui_rect, const char *, unoui_text *, int flags);
    void (*label)   (const struct unoui_theme *, unoui_rect, const char *, int flags);
    void (*progress)(const struct unoui_theme *, unoui_rect, int val, int max);
    void (*vscroll) (const struct unoui_theme *, unoui_rect, int val, int max);
    void (*list)    (const struct unoui_theme *, unoui_rect, const char **, int n, int sel);
    void (*group)   (const struct unoui_theme *, unoui_rect, const char *);
    void (*sep)     (const struct unoui_theme *, unoui_rect);
    void (*icon)    (const struct unoui_theme *, unoui_rect, const char *, int flags);
    /* interactive additions (all NULL-fallback to the defaults) */
    void (*textarea)(const struct unoui_theme *, unoui_rect, unoui_text *, int flags);
    void (*hscroll) (const struct unoui_theme *, unoui_rect, int val, int max);
    void (*slider)  (const struct unoui_theme *, unoui_rect, int v, int vmin, int vmax, int flags);
    void (*spinner) (const struct unoui_theme *, unoui_rect, int v, int flags);
    void (*dropdown)(const struct unoui_theme *, unoui_rect, const char *, int flags);
    void (*tabs)    (const struct unoui_theme *, unoui_rect, const char **, int n, int sel, int flags);
    void (*menubar) (const struct unoui_theme *, unoui_rect, const unoui_menu *, int n, int open, int hot);
    void (*popup)   (const struct unoui_theme *, unoui_rect, const char **, int n, int hot);
} unoui_draw;

typedef struct unoui_theme {
    const char        *name;
    unoui_palette      pal;
    unoui_metrics      m;
    const unoui_draw  *draw;   /* NULL = use all defaults                     */
} unoui_theme;

/* ---- shared drawing helpers (live in unoui.c) ---------------------------- *
 * Themes build their custom graphics from these, so chrome stays compact and
 * automatically depth-correct. */

/* bounds-checked single pixel */
void ui_px(int x, int y, fb_px c);

/* A SHADE 0..UI_SHADES-1 (dark..light) between two endpoint colours. At full
 * depth this is a blended colour; at 1-bit it is an ordered-dither stipple of
 * `a` and `b`. THIS is the bit-depth bridge - write shaded chrome once, it
 * renders right on every panel. */
#define UI_SHADES 5
void ui_shade(int x, int y, int w, int h, const unoui_theme *t,
              fb_px a, fb_px b, int shade);

/* 2-colour ordered-dither fill (50% = classic Mac desktop grey on 1-bit). */
void ui_stipple(int x, int y, int w, int h, fb_px a, fb_px b, int density);

/* raised (lifted>0) or sunken (lifted<0) bevel of thickness `t` around a rect,
 * using the theme's light/shadow/dark roles. Returns the inset content rect. */
unoui_rect ui_bevel(unoui_rect r, const unoui_theme *th, int thick, int lifted);

/* 1px frame whose corners are clipped to fake a `radius`-rounded rectangle. */
void ui_round_frame(unoui_rect r, int radius, fb_px c);
void ui_round_fill (unoui_rect r, int radius, fb_px c);

/* centre / left text within a rect, vertically centred on the ACTIVE font's
 * line height (fb_text_h) - not a hard-coded 8px cell. */
void ui_text_in(unoui_rect r, const char *s, fb_px fg, long bg, int center);

/* ---- font-derived metrics ------------------------------------------------- *
 * All row/line pitches follow the active font (fb_text_h): with the classic
 * 8px bitmap font they equal the historic constants (10/11/12/15/18), with a
 * TrueType face they grow so nothing overlaps or crowds. Widgets are laid out
 * and hit-tested with the SAME functions, so clicks always land where pixels
 * were drawn. */
int ui_line_h(void);            /* multi-line text pitch  (bitmap: 10) */
int ui_row_h(void);             /* list row pitch         (bitmap: 11) */
int ui_prow_h(void);            /* popup/menu row pitch   (bitmap: 12) */
int ui_menubar_h(void);         /* menu bar height        (bitmap: 15) */
int ui_tab_h(void);             /* tab strip height       (bitmap: 18) */
int ui_ctl_h(void);             /* button height          (bitmap: 16) */
int ui_field_h(void);           /* field/spinner/dropdown (bitmap: 16) */
int ui_seg_w(const char *buf, int s, int e);  /* width of buf[s..e), active font */

/* ---- text geometry (shared by the edit painter and the input layer, so a
 * mouse click lands on exactly the glyph that was drawn) ------------------- */
#define UI_LINE_H (ui_line_h())        /* line pitch for multi-line text      */
/* map a pointer (px,py, screen coords) to a caret index within `inner` */
int  ui_text_index_at(unoui_rect inner, const unoui_text *t, int px, int py);
/* map a caret index to its top-left pixel (screen coords) within `inner` */
void ui_text_caret_xy(unoui_rect inner, const unoui_text *t, int idx, int *cx, int *cy);
/* scroll so the caret is visible inside `inner` */
void ui_text_reveal(unoui_rect inner, unoui_text *t);
/* the inner (post-bevel) rect a field/textarea draws its text into */
unoui_rect ui_edit_inner(unoui_rect r, const unoui_theme *th);
/* the default editable-text painter (selection runs, caret, clip) - exported
 * so custom theme field painters can draw the text identically. */
void ui_draw_edit_text(unoui_rect inner, const unoui_text *t,
                       const unoui_theme *th, int focused, int caret_on);

/* font-following heights (kept out of unoui_metrics so the 8 positional theme
 * initialisers stay untouched) */
#define UI_MENUBAR_H (ui_menubar_h())
#define UI_TAB_H     (ui_tab_h())

/* index of the menubar title under px (-1 if none); *tx gets its left x */
int unoui_menubar_index_at(const unoui_theme *, unoui_rect r,
                           const unoui_menu *, int n, int px, int *tx);

/* the portable default painters + the default vtable (the house UnoDOS look) */
extern const unoui_draw unoui_default_draw;

/* ---- the themes shipped to exercise the toolkit -------------------------- */
extern const unoui_theme theme_unodos;    /* default unified UnoDOS look       */
extern const unoui_theme theme_macos7;    /* Mac OS System 7 "Platinum"        */
extern const unoui_theme theme_macplus;   /* Mac System 1-6 / Plus, 1-bit B&W  */
extern const unoui_theme theme_win31;     /* Windows 3.1 grey 3D               */
extern const unoui_theme theme_amiga;     /* Amiga Workbench 1.x               */
extern const unoui_theme theme_c64;       /* Commodore 64 (blue PETSCII)       */
extern const unoui_theme theme_apple2;    /* Apple II (green phosphor mono)    */
extern const unoui_theme theme_next;      /* NeXTSTEP chiselled greyscale      */
extern const unoui_theme theme_aurora_light; /* modern flat/rounded, light      */
extern const unoui_theme theme_aurora_dark;  /* modern flat/rounded, dark       */

/* Aurora compositing level. 0 = FULL (per-frame soft drop shadows + AA rounded
 * corners); 1 = LITE/STATIC (opaque squared chrome, no per-frame alpha) for
 * hardware that can't composite live. The desktop gradient/blobs are unaffected
 * (they are baked into the cached background either way). Set it, then request a
 * repaint. */
extern int unoui_aurora_lite;

#endif /* UNOUI_THEME_H */

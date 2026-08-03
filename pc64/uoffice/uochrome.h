/* ===========================================================================
 * uochrome - the Office 97 command-bar engine.            [EXPERIMENTAL]
 *
 * The shared chrome UnoWord, UnoCalc and UnoShow all render through
 * (docs/OFFICE97-PLAN.md §5 phase 6; the conformance items are
 * docs/OFFICE97-SPEC.md S-OFF-01 and S-OFF-02).
 *
 * WHY THIS IS NOT unoui.  Office 97's menus and toolbars are not native
 * controls - they are owner-drawn "command bars", which is the one lucky
 * thing about cloning this era: we are supposed to draw them ourselves.  And
 * we would have to anyway, because unoui's menubar is flat (no submenus,
 * separators, icons, checkmarks or accelerator column) and its 64-widget
 * ceiling could not host an Office toolbar row.  So the suite draws its own
 * chrome inside ONE UI_CANVAS, exactly as UnoAmp draws Winamp's window, and
 * consumes unoui as a neutral API rather than changing it.
 *
 * PURE FUNCTION OF THE EVENT STREAM, like unoui's input layer: every gesture
 * goes through uoc_handle(), so the host storyboard gate renders exactly what
 * the OS renders from the same events.  Drawing is fb.h only, so this file is
 * identical on the host harness and on pc64 (where fb_text is the TTF engine
 * rather than the 8x8 bitmap - the metrics below are derived from
 * fb_text_h()/fb_text_w() and never hardcoded).
 *
 * PHASE 6a (this file): the menu bar, full static menus, and docked toolbars
 * of flat buttons.  Floating/docking, combo boxes and tear-off palettes are
 * 6b; the dialog engine is 6c; the file dialog, status bar and Assistant are
 * 6d.
 * ======================================================================== */
#ifndef UOCHROME_H
#define UOCHROME_H

#include "fb.h"
#include "unoui.h"          /* the event contract only: unoui_event, UI_* */

/* ---- the look, in one table -----------------------------------------------
 * Every colour and gap the chrome draws with, so the whole suite re-tunes
 * from one place once the pixel-check against a real Office 97 install
 * happens (SPEC: items tagged "(verify)").  The defaults are the Windows
 * 95 / NT4 system colours Office 97 shipped against. */
typedef struct {
    fb_px face;        /* #C0C0C0 - every bar and popup background           */
    fb_px hilight;     /* #FFFFFF - the bright edge of a raised bevel        */
    fb_px light;       /* #DFDFDF - its outer, softer edge                   */
    fb_px shadow;      /* #808080 - the dark edge                            */
    fb_px dkshadow;    /* #000000 - the outermost dark edge                  */
    fb_px text;        /* #000000                                            */
    fb_px gray_text;   /* #808080 - disabled, drawn with a white emboss      */
    fb_px sel;         /* #000080 - the navy selection bar                   */
    fb_px sel_text;    /* #FFFFFF                                            */
    int   icon_px;     /* 16 - the atlas cell, and the icon gutter's content */
    int   pad;         /* text inset inside a bar item                       */
} uoc_look;

/* The Office 97 defaults. */
const uoc_look *uoc_look_97(void);

/* ---- menus -----------------------------------------------------------------
 * Menus are static data: an app declares its whole menu tree as const tables
 * and hands them over.  '&' marks the mnemonic (always underlined - Windows
 * 95 showed them unconditionally; hiding them until Alt is a Windows 2000
 * behaviour and would be wrong here).  '\t' splits the label from its
 * accelerator, which is drawn right-aligned in its own column.
 *
 * A separator is an item with text == NULL. */
enum {
    UOC_DISABLED = 1,   /* greyed, with the Win95 white emboss, never fires  */
    UOC_CHECKED  = 2,   /* a check in the icon gutter, or a sunken icon      */
    UOC_RADIO    = 4,   /* a bullet rather than a check                      */
    UOC_TOGGLE   = 8    /* toolbar: a two-state button (draws sunken when on)*/
};

typedef struct uoc_item {
    const char *text;              /* "&Open...\tCtrl+O"; NULL = separator   */
    int         id;                /* the command uoc_handle reports; 0 none */
    int         icon;              /* atlas cell, -1 for none                */
    unsigned    flags;             /* UOC_*                                  */
    const struct uoc_item *sub;    /* a submenu, or NULL                     */
    int         nsub;
} uoc_item;

typedef struct { const char *title; const uoc_item *item; int n; } uoc_menu;

/* ---- toolbars --------------------------------------------------------------
 * Phase 6a covers buttons, toggles and separators; UOC_TB_COMBO and
 * UOC_TB_SPLIT are declared now so 6b can fill them in without moving the
 * enum out from under anyone (an additive seam, per AGENTS.md §2). */
enum { UOC_TB_BUTTON = 0, UOC_TB_TOGGLE, UOC_TB_SEP, UOC_TB_COMBO, UOC_TB_SPLIT };

typedef struct {
    int         kind;              /* UOC_TB_*                               */
    int         id;
    int         icon;
    const char *tip;               /* ScreenTip text (6b draws it)           */
    unsigned    flags;             /* UOC_DISABLED                           */
    int         w;                 /* 0 = the natural width for its kind     */
} uoc_tbitem;

typedef struct { const char *name; const uoc_tbitem *item; int n; } uoc_tbar;

/* F10 activates the menu bar.  unoui's virtual-key enum has no function keys,
 * and widening someone else's enum for our lane would be exactly the
 * choke-point edit AGENTS.md §2 forbids - so the constant lives here and the
 * app maps its platform's F10 onto it. */
enum { UOC_KEY_F10 = 0x210 };

/* ---- the live chrome ------------------------------------------------------- */
#define UOC_MAXDEPTH 4             /* menu -> submenu -> submenu -> submenu  */
#define UOC_MAXTOGGLE 64

typedef struct {
    /* what the app declared */
    const uoc_look *look;
    const uoc_menu *menu;  int nmenu;
    const uoc_tbar *tbar;  int ntbar;   /* drawn as rows, in order           */
    int x, y, w;                        /* where the chrome sits             */

    /* live interaction state - all of it derived from the event stream */
    int open;                           /* open top-level menu, -1 = none    */
    int hot;                            /* hovered top-level menu, -1 = none */
    int depth;                          /* how many popup levels are open    */
    int path[UOC_MAXDEPTH];             /* the hot item at each level        */
    int keyed;                          /* keyboard-driven (F10 / Alt) mode  */
    int hot_bar, hot_btn;               /* hovered toolbar button            */
    int down_bar, down_btn;             /* the button the mouse is holding   */
    struct { int id, on; } toggle[UOC_MAXTOGGLE];
    int ntoggle;
} uoc_ui;

/* Set up a chrome over `menu`/`tbar`, positioned at (x,y) and `w` wide. */
void uoc_init(uoc_ui *u, const uoc_menu *menu, int nmenu,
              const uoc_tbar *tbar, int ntbar, int x, int y, int w);

/* How tall the whole chrome is: the menu bar plus one row per toolbar.  The
 * app's client area starts at u->y + uoc_height(u). */
int  uoc_height(const uoc_ui *u);

/* Toggle-button state.  The app owns what a toggle MEANS; uochrome only
 * remembers whether it is currently down, so a bold button can be drawn from
 * the caret's run without the app re-declaring its tables. */
void uoc_toggle_set(uoc_ui *u, int id, int on);
int  uoc_toggle(const uoc_ui *u, int id);

/* One event.  Returns 1 if the chrome consumed it (the app must not also act
 * on it).  When a command fires, *cmd is set to its id; otherwise 0. */
int  uoc_handle(uoc_ui *u, const unoui_event *e, int *cmd);

/* Draw the bars, and any open menu on top of them.  The popup deliberately
 * paints last and outside the bar rect, exactly as a real menu overlaps the
 * document beneath it. */
void uoc_render(const uoc_ui *u);

/* Is a menu open?  The app uses this to know its own canvas is obscured. */
int  uoc_menu_open(const uoc_ui *u);

/* Close everything (Esc, a click elsewhere, or the app switching document). */
void uoc_dismiss(uoc_ui *u);

/* ---- the icon atlas (an installation seam, filled in by phase 6b) ----------
 * One sprite sheet of `cell`x`cell` cells, `cols` across, in fb_px order.
 * Until artwork is installed every icon draws as a neutral placeholder, so
 * layout and behaviour are testable before a single pixel is drawn. */
void uoc_set_icons(const fb_px *atlas, int cell, int cols, int count);

#endif /* UOCHROME_H */

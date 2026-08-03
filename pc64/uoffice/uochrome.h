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
 * rather than the 8x8 bitmap - the metrics are derived from fb_text_h() /
 * fb_text_w() and never hardcoded).
 *
 * PHASE 6a: the menu bar, static menus, docked toolbars of flat buttons.
 * PHASE 6b: docking on all four edges and floating, combo lists, split
 *           buttons with tear-off palettes, ScreenTips, and the icon atlas
 *           (uoicons.h) filling the seam 6a left open.
 * The dialog engine is uodlg.h (6c); the status bar, ruler and Assistant are
 * uobars.h (6d).
 * ======================================================================== */
#ifndef UOCHROME_H
#define UOCHROME_H

#include "fb.h"
#include "unoui.h"          /* the event contract only: unoui_event, UI_* */

/* The screen extent, as FUNCTIONS.  pc64's FB_W/FB_H expand to the
 * variables uno_fb_w/uno_fb_h, and a .UNO module can only import functions -
 * the loader turns every undefined symbol into a jmp thunk.  So the whole
 * lane calls these and never the macros; pc64 exports them, and the host
 * harness gets them from uoffice/host_fbdim.c. */
int fb_width(void);
int fb_height(void);

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
    fb_px tip_bg;      /* #FFFFE1 - the ScreenTip's pale yellow              */
    int   icon_px;     /* 16 - the atlas cell, and the icon gutter's content */
    int   pad;         /* text inset inside a bar item                       */
} uoc_look;

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

/* ---- palettes (phase 6b) ---------------------------------------------------
 * What a split button drops: a grid of colour swatches (Font Color, Fill
 * Color, Highlight) or of icons (Borders).  Every one of them can be TORN
 * OFF - dragged away by the move bar across its top - and left floating,
 * which is the Office 97 gesture people remember. */
enum { UOC_PAL_COLOR = 0, UOC_PAL_ICON };

typedef struct {
    int          kind;             /* UOC_PAL_*                              */
    const fb_px *color;            /* UOC_PAL_COLOR: the swatches            */
    const int   *icon;             /* UOC_PAL_ICON: atlas cells              */
    int          n, cols;
    const char  *title;            /* shown on the bar once torn off         */
} uoc_palette;

/* ---- toolbars --------------------------------------------------------------
 * UOC_TB_COMBO carries a list (Style, Font, Size, Zoom); UOC_TB_SPLIT carries
 * a palette behind its arrow. */
enum { UOC_TB_BUTTON = 0, UOC_TB_TOGGLE, UOC_TB_SEP, UOC_TB_COMBO, UOC_TB_SPLIT };

typedef struct {
    int         kind;              /* UOC_TB_*                               */
    int         id;
    int         icon;
    const char *tip;               /* the ScreenTip                          */
    unsigned    flags;             /* UOC_DISABLED                           */
    int         w;                 /* 0 = the natural width for its kind     */
    const char *const *list;       /* UOC_TB_COMBO entries                   */
    int         nlist;
    const uoc_palette *pal;        /* UOC_TB_SPLIT                           */
} uoc_tbitem;

typedef struct { const char *name; const uoc_tbitem *item; int n; } uoc_tbar;

/* Where a toolbar lives.  Office 97 docks on all four edges and floats; a
 * band is a row (top/bottom) or column (left/right), and two bars sharing a
 * band sit side by side in declaration order. */
enum { UOC_DOCK_TOP = 0, UOC_DOCK_BOTTOM, UOC_DOCK_LEFT, UOC_DOCK_RIGHT,
       UOC_DOCK_FLOAT };

typedef struct {
    int dock;                      /* UOC_DOCK_*                             */
    int band;                      /* which row/column within that edge      */
    int fx, fy;                    /* where it floats                        */
    int hidden;                    /* closed from a floating bar's X         */
} uoc_barstate;

/* F10 activates the menu bar.  unoui's virtual-key enum has no function keys,
 * and widening someone else's enum for our lane would be exactly the
 * choke-point edit AGENTS.md §2 forbids - so the constant lives here and the
 * app maps its platform's F10 onto it. */
enum { UOC_KEY_F10 = 0x210 };

/* ---- the live chrome ------------------------------------------------------- */
#define UOC_MAXDEPTH  4            /* menu -> submenu -> submenu -> submenu  */
#define UOC_MAXTOGGLE 64
#define UOC_MAXBARS   8
#define UOC_MAXTEAR   4            /* palettes torn off at once              */
#define UOC_TIP_TICKS 8            /* hover ticks before a ScreenTip shows   */

/* what a drop-down currently is */
enum { UOC_POP_NONE = 0, UOC_POP_COMBO, UOC_POP_PALETTE };

typedef struct {
    const uoc_palette *pal;
    int x, y, open, id;
} uoc_tearoff;

typedef struct {
    /* what the app declared */
    const uoc_look *look;
    const uoc_menu *menu;  int nmenu;
    const uoc_tbar *tbar;  int ntbar;
    int x, y, w, h;                     /* the frame the chrome lays out in  */

    /* live interaction state - all of it derived from the event stream */
    int open;                           /* open top-level menu, -1 = none    */
    int hot;                            /* hovered top-level menu, -1 = none */
    int depth;                          /* how many popup levels are open    */
    int path[UOC_MAXDEPTH];             /* the hot item at each level        */
    int keyed;                          /* keyboard-driven (F10 / Alt) mode  */
    int hot_bar, hot_btn;               /* hovered toolbar button            */
    int down_bar, down_btn;             /* the button the mouse is holding   */

    uoc_barstate bs[UOC_MAXBARS];

    int pop_kind;                       /* UOC_POP_*                         */
    int pop_bar, pop_btn, pop_hot, pop_pick;

    int drag_bar;                       /* the toolbar being dragged, -1     */
    int drag_dx, drag_dy, drag_x, drag_y;
    int drag_tear;                      /* the tear-off being dragged, -1    */

    uoc_tearoff tear[UOC_MAXTEAR];

    int tip_bar, tip_btn, tip_ticks;    /* ScreenTip timing                  */

    struct { int id, on; } toggle[UOC_MAXTOGGLE];
    int ntoggle;
    struct { int id, sel; } combo[UOC_MAXTOGGLE];
    int ncombo;
} uoc_ui;

/* Set up a chrome over `menu`/`tbar` inside the frame (x,y,w,h).  Every
 * toolbar starts docked at the top, one per band, in declaration order. */
void uoc_init(uoc_ui *u, const uoc_menu *menu, int nmenu,
              const uoc_tbar *tbar, int ntbar, int x, int y, int w, int h);

/* Move a toolbar.  `band` is ignored when floating. */
void uoc_dock(uoc_ui *u, int bar, int dock, int band, int fx, int fy);

/* What is left for the document once every docked band is accounted for. */
void uoc_client_rect(const uoc_ui *u, int *x, int *y, int *w, int *h);

/* The height of the TOP band alone - the common case an app wants when it
 * only ever docks at the top. */
int  uoc_height(const uoc_ui *u);

/* Toggle-button state.  The app owns what a toggle MEANS; uochrome only
 * remembers whether it is currently down, so a bold button can be drawn from
 * the caret's run without the app re-declaring its tables. */
void uoc_toggle_set(uoc_ui *u, int id, int on);
int  uoc_toggle(const uoc_ui *u, int id);

/* Combo selection, by the same contract. */
void uoc_combo_set(uoc_ui *u, int id, int sel);
int  uoc_combo(const uoc_ui *u, int id);

/* One event.  Returns 1 if the chrome consumed it (the app must not also act
 * on it).  When a command fires, *cmd is set to its id; otherwise 0.  A
 * palette pick reports the split button's id, and the chosen entry is read
 * back with uoc_pick(). */
int  uoc_handle(uoc_ui *u, const unoui_event *e, int *cmd);

/* The index picked out of the last palette or combo list. */
int  uoc_pick(const uoc_ui *u);

/* Draw the bars, then anything floating, then any open menu or drop-down.
 * Popups deliberately paint last and outside the bar rect, exactly as a real
 * menu overlaps the document beneath it. */
void uoc_render(const uoc_ui *u);
/* The same paint in two halves, for an app whose content sits BELOW the
 * chrome: bars, then the app's own content, then the popups LAST.  Calling
 * uoc_render() in that position clips every dropdown to the toolbar band. */
void uoc_render_bars(const uoc_ui *u);
void uoc_render_popups(const uoc_ui *u);

int  uoc_menu_open(const uoc_ui *u);
void uoc_dismiss(uoc_ui *u);

/* ---- the icon atlas (installed by uoicons.h) ------------------------------
 * One sprite sheet of `cell`x`cell` cells, `cols` across, in fb_px order.
 * Until artwork is installed every icon draws as a neutral placeholder, so
 * layout and behaviour are testable before a single pixel is drawn. */
void uoc_set_icons(const fb_px *atlas, int cell, int cols, int count);

/* Draw one atlas cell.  Exposed because the dialog engine and the Assistant
 * draw the same icons outside a command bar. */
void uoc_draw_icon(const uoc_look *k, int x, int y, int idx, int disabled);

/* The two Windows 95 edges the whole suite is made of, shared with uodlg. */
void uoc_raised(const uoc_look *k, int x, int y, int w, int h);
void uoc_sunken(const uoc_look *k, int x, int y, int w, int h);
void uoc_bevel (int x, int y, int w, int h, fb_px tl, fb_px br, int thick);
void uoc_etch_h(const uoc_look *k, int x, int y, int w);
void uoc_etch_v(const uoc_look *k, int x, int y, int h);

/* Draw a label, dropping '&' and underlining the character it marked; stops
 * at '\t'.  Returns the width drawn.  Shared with uodlg, whose control labels
 * carry mnemonics too. */
int  uoc_draw_label(int x, int y, const char *s, fb_px fg, int show_mn);
int  uoc_label_w(const char *s);
int  uoc_mnemonic_of(const char *s);

#endif /* UOCHROME_H */

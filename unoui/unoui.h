/* ===========================================================================
 * unoui - the UnoDOS cross-platform UI toolkit.
 *
 * Write an app's UI ONCE, render + drive it on every platform with a unified
 * look - or swap a per-platform THEME to make it native. Mirrors uno3d: a
 * portable core over the shared `fb.h` software framebuffer plus a swappable
 * vtable. uno3d swaps the rasteriser BACKEND; unoui swaps the THEME.
 *
 * INPUT IS PORTABLE BY CONSTRUCTION. The toolkit's behaviour is a pure function
 * of an abstract event stream (unoui_event). Each port writes ONE tiny adapter
 * mapping its native mouse/keyboard to unoui_event and calls unoui_handle();
 * identical events produce identical behaviour everywhere - drag, multi-line
 * text entry, focus traversal, menus. That adapter + the fb hookup is the only
 * per-platform code an app needs.
 *
 *   1. App builds windows + widgets once          (unoui_window / unoui_add_*)
 *   2. Port feeds events                          (unoui_handle(&ui, &ev))
 *   3. Toolkit renders desktop+windows+popups     (unoui_render_ui(&ui))
 *   4. A theme restyles all of it                 (colours AND graphics)
 * ===========================================================================
 */
#ifndef UNOUI_H
#define UNOUI_H

#include "fb.h"

/* ---- widget kinds -------------------------------------------------------- */
typedef enum {
    UI_LABEL, UI_BUTTON, UI_CHECK, UI_RADIO,
    UI_FIELD,      /* single-line text: static, or editable if .edit set      */
    UI_PROGRESS, UI_VSCROLL, UI_LIST, UI_GROUP, UI_SEP, UI_ICON,
    UI_TEXTAREA,   /* multi-line editable text                                */
    UI_HSCROLL,    /* horizontal scrollbar                                    */
    UI_SLIDER,     /* draggable knob over a track (vmin..vmax)                */
    UI_SPINNER,    /* numeric stepper with up/down arrows                     */
    UI_DROPDOWN,   /* closed combo; opens a popup list                        */
    UI_TABS,       /* row of tab headers; sel = active                        */
    UI_MENUBAR,    /* row of menu titles; each opens a popup of items         */
    UI_CANVAS,     /* app-drawn region: the app renders into fb inside .r     */
    UI_MDI,        /* container of draggable child frames (appended - see §MDI) */
    UI_BUSY        /* indeterminate "working" indicator (appended - see below)  */
} ui_kind;

/* ---- per-widget state flags --------------------------------------------- */
enum {
    UI_F_DEFAULT  = 1 << 0,   /* default/affirmative button (gets a ring)     */
    UI_F_PRESSED  = 1 << 1,   /* shown held down                              */
    UI_F_FOCUS    = 1 << 2,   /* has keyboard focus                           */
    UI_F_DISABLED = 1 << 3,   /* greyed out, not interactive                  */
    UI_F_CHECKED  = 1 << 4,   /* checkbox/radio set                           */
    UI_F_CARET    = 1 << 5,   /* draw the text caret this frame (blink on)    */
    UI_F_HOT      = 1 << 6    /* mouse hovering                               */
};

typedef struct { int x, y, w, h; } unoui_rect;

/* ---- editable text model (shared by UI_FIELD and UI_TEXTAREA) ------------ *
 * The app owns the char buffer; the toolkit edits it in place and tracks the
 * caret + selection. Multi-line stores '\n' in the buffer. */
typedef struct {
    char *buf;        /* app-owned, NUL-terminated                            */
    int   cap;        /* buffer capacity incl. the NUL                        */
    int   len;        /* current length                                       */
    int   caret;      /* caret index 0..len                                   */
    int   sel;        /* selection anchor; sel==caret means no selection      */
    int   scroll_x;   /* horizontal view offset, px                           */
    int   scroll_y;   /* vertical view offset, px (multi-line)                */
    int   multiline;
    /* --- appended; 0 must mean "as before" -------------------------------- */
    int   secret;     /* mask character (0 = plain text). See below.          */
    int   revealed;   /* showing the real text right now; cleared on blur     */
} unoui_text;

void unoui_text_init(unoui_text *t, char *buf, int cap, int multiline);
void unoui_text_set (unoui_text *t, const char *s);

/* ---- secret (password) fields ---------------------------------------------
 * `secret` is the character the field DRAWS in place of every real one; the
 * model still holds the real text, so the caret, the selection, the length and
 * everything the app reads are the password, not the mask. That is the whole
 * point of doing this in the toolkit: the alternative - feeding the widget
 * '*' and keeping the real characters in a side buffer - makes every one of
 * those belong to the mask instead, and every app that wants a password field
 * has to reimplement it.
 *
 * Measurement follows the mask, so a click and the caret land on the glyph
 * under the pointer exactly as they do in a plain field.
 *
 * REVEAL. A masked field with no way to check what you typed is how a correct
 * passphrase gets retyped four times. A secret field therefore draws a small
 * eye at its right-hand end; clicking it shows the real text, clicking again
 * hides it. It is deliberately TEMPORARY: it clears the moment the field loses
 * focus, so a password is never left readable on a screen somebody has walked
 * away from. Apps can drive it themselves with unoui_text_show().
 *
 * (Not to be confused with ui_text_reveal(), which scrolls a field so the
 * caret is visible - an older and unrelated use of the word.)
 *
 * Multi-line and secret are mutually exclusive (a masked paragraph is not a
 * thing); unoui_text_secret() clears `multiline`. */
void unoui_text_secret(unoui_text *t, int mask_char);  /* 0 = back to plain   */
void unoui_text_show  (unoui_text *t, int on);   /* show the real text  */
/* The eye's rect inside a field's inner rect - {0,0,0,0} when the field is not
 * secret. Shared by the painter and the hit test so a click on the eye can
 * never be off by a pixel from the eye that was drawn. */
unoui_rect ui_edit_eye_rect(unoui_rect inner, const unoui_text *t);
/* the part of the inner rect the TEXT gets - inner minus the eye. Every
 * measurement about an editable field is made against this one. */
unoui_rect ui_edit_text_rect(unoui_rect inner, const unoui_text *t);

/* ---- menus (for UI_MENUBAR and UI_DROPDOWN popups) ----------------------- */
typedef struct unoui_menu {
    const char  *title;
    const char **items;
    int          nitems;
} unoui_menu;

/* ---- canvas (UI_CANVAS): an app-drawn region -----------------------------
 * The toolkit does the window chrome, focus, drag and z-order; the app owns
 * the pixels inside the canvas rect. `draw` is called each render with the
 * canvas's absolute screen rect (the whole screen in fullscreen); `event`
 * receives input while the canvas is focused (or always, in fullscreen). This
 * is how games / paint / tracker render custom graphics inside the same
 * desktop, and pairs with unoui_fullscreen() for full-screen apps. */
struct unoui_widget;
typedef struct unoui_canvas {
    void (*draw)(struct unoui_widget *w, unoui_rect r, void *ctx);
    int  (*event)(struct unoui_widget *w, const void *ev, void *ctx); /* ev = unoui_event* */
    void *ctx;
} unoui_canvas;

struct unoui_mdi;             /* fwd (defined with the MDI section below) */

/* A single widget. Geometry `r` is relative to the window's CONTENT origin. */
typedef struct unoui_widget {
    ui_kind      kind;
    unoui_rect   r;
    const char  *text;        /* label / caption (static)                     */
    int          id;          /* app-assigned id, echoed back in unoui_action */
    int          flags;       /* UI_F_*                                        */
    int          value, vmin, vmax;
    const char **items;       /* list / dropdown / tabs items                 */
    int          nitems;
    int          sel;         /* selected index                               */
    unoui_text  *edit;        /* non-NULL => editable text widget             */
    const unoui_menu *menus;  /* menubar: array of menus                      */
    int          nmenus;
    unoui_canvas *canvas;     /* non-NULL => app-drawn UI_CANVAS              */
    int          icon;        /* UI_ICON: art id for the icon-art hook        */
    /* --- appended; 0 must mean "as before" -------------------------------- */
    struct unoui_mdi *mdi;    /* UI_MDI: the app-owned child set              */
    int          dx;          /* transient x offset, px - see unoui_reject_*  */
} unoui_widget;

/* Optional per-app icon artwork. When set, UI_ICON widgets are drawn by this
 * hook (given the icon's full rect, its `icon` id, label and flags) instead of
 * the theme's generic glyph - so a port can supply distinct app icons. */
typedef void (*unoui_icon_fn)(int icon, unoui_rect r, const char *label, int flags);
extern unoui_icon_fn unoui_icon_art;

/* Optional desktop wallpaper hook. When set, it paints the whole-screen
 * backdrop instead of the active theme's desktop painter. Return non-zero if it
 * painted (theme desktop skipped), 0 to fall through to the theme's own desktop
 * (the "theme default" wallpaper). Mirrors unoui_icon_art: a port supplies it,
 * the toolkit calls it. Invalidate the cached backdrop via unoui_bg_invalidate()
 * whenever the selection changes. */
struct unoui_theme;           /* fwd (defined in unoui_theme.h) */
typedef int (*unoui_wallpaper_fn)(const struct unoui_theme *t, int W, int H);
extern unoui_wallpaper_fn unoui_wallpaper;

#define UNOUI_MAX_WIDGETS 64

/* ---- window flags (unoui_window.flags) ---------------------------------- *
 * BARE strips the frame + titlebar and disables dragging - for shell chrome
 * that isn't a normal app window (the desktop-icon layer, a dock/taskbar). The
 * pin flags fix z-order: a BOTTOM window stays behind every normal window, a
 * TOP window stays in front of them, and raising a normal window on click can
 * never jump above a TOP bar or below a BOTTOM desktop. */
enum {
    UI_WIN_BARE   = 1 << 0,   /* no chrome (frame/titlebar), not draggable    */
    UI_WIN_BOTTOM = 1 << 1,   /* pinned behind normal windows (the desktop)   */
    UI_WIN_TOP    = 1 << 2,   /* pinned in front of normal windows (taskbar)  */
    UI_WIN_RESIZE = 1 << 3,   /* draggable bottom-right grip; fill widgets reflow */
    UI_WIN_NOCTL  = 1 << 4    /* titled, but no minimize/maximize boxes       */
};

/* ---- snap states (unoui_window.snap) ------------------------------------- *
 * Where a window has been snapped to inside the work area. UI_SNAP_NONE = a
 * free-floating window, which is what a zero-initialised window is, so nothing
 * that never snaps has to know these exist. The geometry of each state is pure
 * arithmetic on ui->work - see unoui_snap_rect(). */
enum {
    UI_SNAP_NONE = 0, UI_SNAP_MAX,
    UI_SNAP_L, UI_SNAP_R,                     /* halves                        */
    UI_SNAP_TL, UI_SNAP_TR, UI_SNAP_BL, UI_SNAP_BR   /* quarters               */
};

typedef struct unoui_window {
    const char   *title;
    unoui_rect    r;          /* whole window incl. title bar, screen coords  */
    int           active;     /* 1 = focused window (active title chrome)     */
    int           flags;      /* UI_WIN_*; 0 = normal draggable app window     */
    unoui_widget  w[UNOUI_MAX_WIDGETS];
    int           nw;
    int           content_x;  /* set by the window painter; canonical origin  */
    int           content_y;
    int           font_slot;  /* per-window font override: -2 inherit, -1 bitmap, 0.. TTF */
    int           min_w, min_h; /* resize floor (UI_WIN_RESIZE)                */
    /* --- appended; 0 must mean "as before" (see unoui_snap_apply) ---------- */
    unsigned char snap;       /* UI_SNAP_*: which snap state this window is in */
    unoui_rect    restore_r;  /* the pre-snap rect to give back on un-snap     */
} unoui_window;

struct unoui_theme;           /* fwd (defined in unoui_theme.h) */

/* ---- calendar (reusable core of a date-picker) --------------------------- */
enum { UI_CAL_NONE = 0, UI_CAL_PREV = -1, UI_CAL_NEXT = -2 };
int  unoui_days_in_month(int y, int m);
int  unoui_day_of_week(int y, int m, int d);       /* 0 = Sunday */
void unoui_calendar_draw(const struct unoui_theme *, unoui_rect r, int y, int m, int sel);
/* map a click to a day (1..31) / UI_CAL_PREV / UI_CAL_NEXT / UI_CAL_NONE */
int  unoui_calendar_hit(unoui_rect r, int y, int m, int px, int py);

/* widget flag (persistent, in widget->flags): a fill widget's w/h are stretched
 * to the window content rect on resize, so canvas apps reflow. */
#define UI_WF_FILL (1 << 12)
void unoui_widget_fill(unoui_widget *w);          /* mark a widget as fill */
/* recompute fill widgets' sizes from the window's current content rect */
void unoui_reflow_window(const struct unoui_theme *, unoui_window *);

/* per-window font override hooks (set by the platform; NULL = ignored). When a
 * window's font_slot != UI_FONT_INHERIT, the renderer wraps its widget drawing
 * in push/pop so that window's content uses a different face. */
#define UI_FONT_INHERIT (-2)
extern void (*unoui_font_push)(int slot);
extern void (*unoui_font_pop)(void);

struct unoui_theme;           /* defined in unoui_theme.h */

/* ---- building a window (the write-once app side) ------------------------- */
void unoui_window_init(unoui_window *win, const char *title,
                       int x, int y, int w, int h);

unoui_widget *unoui_add_label (unoui_window *, int x, int y, const char *text);
unoui_widget *unoui_add_button(unoui_window *, int x, int y, int w,
                               const char *text, int flags);
unoui_widget *unoui_add_check (unoui_window *, int x, int y, const char *text, int on);
unoui_widget *unoui_add_radio (unoui_window *, int x, int y, const char *text, int on);
unoui_widget *unoui_add_field (unoui_window *, int x, int y, int w,
                               const char *text, int focus);     /* static    */
unoui_widget *unoui_add_edit  (unoui_window *, int x, int y, int w,
                               unoui_text *t);                   /* editable  */
unoui_widget *unoui_add_textarea(unoui_window *, int x, int y, int w, int h,
                               unoui_text *t);
unoui_widget *unoui_add_progress(unoui_window *, int x, int y, int w, int v, int vmax);
/* An indeterminate BUSY indicator: a ring of dots with the bright one walking
 * round it. UI_PROGRESS answers "how far"; this one answers "is it alive" -
 * the honest report for work whose length nobody knows, and the thing a
 * frozen-looking machine most needs to say.
 *
 * `value` is the animation PHASE, not a fraction: the app advances it (any
 * amount, any cadence - it is taken modulo the dot count) and repaints. That
 * keeps the widget stateless, and it means a BLOCKING caller can step it from
 * inside its own wait loop - which is exactly where the feedback is needed and
 * the one place a timer-driven animation cannot reach.
 *
 * unoui_busy_step() is the convenience: advance one dot, so a wait loop reads
 * `unoui_busy_step(w); repaint();`. */
unoui_widget *unoui_add_busy(unoui_window *, int x, int y, int size);
void          unoui_busy_step(unoui_widget *);
unoui_widget *unoui_add_vscroll(unoui_window *, int x, int y, int h, int v, int vmax);
unoui_widget *unoui_add_hscroll(unoui_window *, int x, int y, int w, int v, int vmax);
unoui_widget *unoui_add_slider(unoui_window *, int x, int y, int w,
                               int vmin, int vmax, int v);
unoui_widget *unoui_add_spinner(unoui_window *, int x, int y, int w,
                               int vmin, int vmax, int v);
unoui_widget *unoui_add_dropdown(unoui_window *, int x, int y, int w,
                               const char **items, int n, int sel);
unoui_widget *unoui_add_tabs  (unoui_window *, int x, int y, int w,
                               const char **items, int n, int sel);
unoui_widget *unoui_add_menubar(unoui_window *, const unoui_menu *menus, int n);
unoui_widget *unoui_add_list  (unoui_window *, int x, int y, int w, int h,
                               const char **items, int n, int sel);
unoui_widget *unoui_add_group (unoui_window *, int x, int y, int w, int h,
                               const char *title);
unoui_widget *unoui_add_sep   (unoui_window *, int x, int y, int w);
unoui_widget *unoui_add_icon  (unoui_window *, int x, int y, const char *text);
unoui_widget *unoui_add_canvas(unoui_window *, int x, int y, int w, int h,
                               unoui_canvas *c);

/* ---- scrolling lists (UI_LIST) -------------------------------------------
 * A list shows a WINDOW of its items instead of clipping at the box edge, so a
 * list longer than its rect is fully reachable: the toolkit keeps the first
 * visible row in the widget's `value`, paints an inline scrollbar on the right
 * edge when the list overflows, and drives it from the wheel, the bar (arrows +
 * thumb drag) and the keyboard (arrows / PgUp / PgDn / Home / End, which keep
 * the selection in view). An app just calls unoui_add_list() with the full item
 * array and reads the selected index out of unoui_action.value.
 *
 * The same geometry is public so a UI_CANVAS app - which owns raw pixels, not
 * widgets - can host an identical list inside its own rect: draw it with
 * unoui_list_draw(), map a click with unoui_list_index_at() / unoui_list_bar(),
 * and keep the view honest with unoui_list_reveal(). */
#define UI_LIST_BAR_W 12                  /* inline scrollbar width, px         */
/* widget flag: the app moved `sel` itself, so the next draw scrolls that row
 * into view and clears the bit. Set it through unoui_list_set_sel(). */
#define UI_WF_LIST_REVEAL (1 << 13)
void unoui_list_set_sel(unoui_widget *w, int sel);  /* select + scroll into view */
int  unoui_list_rows  (unoui_rect r);              /* rows visible in `r`       */
int  unoui_list_maxtop(unoui_rect r, int n);       /* largest legal first row   */
int  unoui_list_index_at(unoui_rect r, int n, int top, int y);  /* row under y  */
int  unoui_list_reveal(unoui_rect r, int n, int sel, int top);  /* clamped top  */
unoui_rect unoui_list_bar(unoui_rect r, int n);    /* bar strip; .w 0 = none    */
void unoui_list_draw  (const struct unoui_theme *, unoui_rect r,
                       const char **items, int n, int sel, int top);

/* ---- tabbed documents (UI_TABS) ------------------------------------------
 * A bare UI_TABS is a strip of labels: it picks one of several PAGES of the
 * same window, and the app rebuilds the window on a switch (the Control Panel).
 * Adding any UI_TF_* flag turns the same widget into a tabbed DOCUMENT control
 * - per-tab close boxes, a trailing "+", equal-width tabs, and a ">>" overflow
 * control when they no longer fit.
 *
 * Every rect comes from ONE layout pass, so the painter and the hit test agree
 * by construction and a click can only land where something was drawn. That is
 * the same rule unoui_titlebtn_rect() enforces for the title-bar buttons, and
 * it is why the geometry is public: a UI_CANVAS app - which owns raw pixels,
 * not widgets - can host an identical strip inside its own rect by calling
 * unoui_tabs_draw() and unoui_tabs_hit() directly. The browser does.
 *
 * The model is a VIEW over the app's own storage: `labels` is borrowed, never
 * copied or freed, so an app whose titles are mutable char arrays just hands
 * over an array of pointers into them.
 *
 * Flags live in the widget's `flags` (high bits, beside UI_WF_*) and in the
 * model's, using the same constants either way. A zero-flag model renders and
 * hit-tests exactly as a plain strip always has, including honouring a theme's
 * own `tabs` painter - so nothing that never sets a flag can notice these. */
#define UI_TF_CLOSE    (1 << 14)  /* per-tab close box                        */
#define UI_TF_PLUS     (1 << 15)  /* trailing "+" (new document)              */
#define UI_TF_ELASTIC  (1 << 16)  /* equal widths that share the strip        */
#define UI_TF_OVERFLOW (1 << 17)  /* ">>" when the tabs do not fit            */
#define UI_TF_ANY (UI_TF_CLOSE | UI_TF_PLUS | UI_TF_ELASTIC | UI_TF_OVERFLOW)

/* elastic width bounds, px - a tab never shrinks past legibility nor grows to
 * fill a wide strip on its own */
#define UI_TAB_MIN_W 46
#define UI_TAB_MAX_W 130

/* what unoui_tabs_hit() found under the pointer */
enum { UI_TAB_NONE = 0, UI_TAB_SEL, UI_TAB_CLOSE, UI_TAB_PLUS, UI_TAB_OVER };

typedef struct unoui_tabs_model {
    const char *const *labels;  /* borrowed, `n` entries                      */
    int n;
    int sel;                    /* active tab                                 */
    int hot;                    /* tab under the pointer, -1 = none           */
    int hot_part;               /* UI_TAB_* the pointer is over               */
    int first;                  /* first visible tab (UI_TF_OVERFLOW)         */
    int flags;                  /* UI_TF_*                                    */
} unoui_tabs_model;

/* fill a model from a UI_TABS widget. The scroll position lives in the
 * widget's `value`, exactly as a list's `top` does, and the flags in its
 * `flags`; a widget has no per-tab hover, so `hot` comes back -1. */
void unoui_tabs_model_of(const unoui_widget *w, unoui_tabs_model *m);

int  unoui_tabs_h(const struct unoui_theme *);          /* strip height, px   */
/* how many tabs are drawn starting at m->first */
int  unoui_tabs_visible(const struct unoui_theme *, unoui_rect r,
                        const unoui_tabs_model *m);
/* largest legal `first`, and the clamp that keeps `sel` on screen (sel < 0 =
 * clamp only). Mirrors unoui_list_maxtop / unoui_list_reveal. */
int  unoui_tabs_maxfirst(const struct unoui_theme *, unoui_rect r,
                         const unoui_tabs_model *m);
int  unoui_tabs_reveal(const struct unoui_theme *, unoui_rect r,
                       const unoui_tabs_model *m, int sel);
/* geometry: absolute index `i`, not a visible slot. A tab scrolled out of view
 * or absent gets a zero-width rect, exactly as unoui_titlebtn_rect does for a
 * button a window has opted out of. */
unoui_rect unoui_tab_rect      (const struct unoui_theme *, unoui_rect r,
                                const unoui_tabs_model *m, int i);
unoui_rect unoui_tab_close_rect(const struct unoui_theme *, unoui_rect r,
                                const unoui_tabs_model *m, int i);
unoui_rect unoui_tabs_plus_rect(const struct unoui_theme *, unoui_rect r,
                                const unoui_tabs_model *m);
unoui_rect unoui_tabs_over_rect(const struct unoui_theme *, unoui_rect r,
                                const unoui_tabs_model *m);
void unoui_tabs_draw(const struct unoui_theme *, unoui_rect r,
                     const unoui_tabs_model *m);
/* returns UI_TAB_* and writes the tab index through `which` (-1 when the hit
 * was not on a tab). Pass NULL for `which` if you only want the part. */
int  unoui_tabs_hit (const struct unoui_theme *, unoui_rect r,
                     const unoui_tabs_model *m, int x, int y, int *which);

/* ---- MDI: child frames inside a widget -----------------------------------
 * A UI_MDI widget hosts draggable, resizable, closable child frames inside its
 * own rect - the "multiple document" container an editor or a terminal app
 * wants for its panes.
 *
 * A child is NOT a window. It is not in ui->win[], so it never reaches the
 * taskbar, Alt-Tab, virtual desktops or the snap zones, and it cannot be
 * dragged out onto the desktop. That is the deliberate trade: unoui_window is
 * a flat entry in a fixed array shared with the PS2 port, the Dreamcast port
 * and the host demo, and giving it a parent pointer would change the meaning
 * of that array for every one of them. A container widget keeps the whole
 * feature inside one widget kind.
 *
 * What a child DOES get, free and per-theme, is real window chrome: the draw
 * path builds a temporary unoui_window for each child and hands it to the
 * theme's own window and title-bar painters, so frames, title bars, close
 * boxes and resize grips match the desktop on all ten themes with no new
 * artwork. Children carry UI_WIN_NOCTL - minimize and maximize mean nothing
 * without a taskbar and a work area - so no theme draws a control that would
 * do nothing.
 *
 * A child has a close box exactly when the THEME has one (`unoui_metrics
 * .closebox`), and it is always live. There is no per-child opt-out because
 * the close box is drawn by the theme's title-bar painter, which takes no such
 * flag; a child-level flag could only have suppressed the click, leaving a
 * drawn control that did nothing.
 *
 * Geometry is stored RELATIVE to the container rect, so moving or reflowing
 * the container carries its children with it. */
#define UNOUI_MDI_MAX  12
#define UI_MDI_MIN_W   80        /* child resize floor when the app sets none */
#define UI_MDI_MIN_H   48

enum { UI_MDI_RESIZE = 1 << 0 };  /* this child gets a resize grip */

typedef struct unoui_mdi_child {
    unoui_rect    r;        /* RELATIVE to the container rect origin          */
    const char   *title;    /* borrowed, never copied or freed                */
    int           flags;    /* UI_MDI_*                                       */
    unoui_canvas *canvas;   /* optional content painter + event handler       */
    int           used;
} unoui_mdi_child;

typedef struct unoui_mdi {
    unoui_mdi_child *ch;    /* app-owned array of `cap` entries               */
    int cap;
    int min_w, min_h;       /* child resize floor; 0 = UI_MDI_MIN_*           */
    /* z-order back..front, and the focused child. Both hold index + 1, with 0
     * meaning "none" - because 0 is a valid child index and a zero-initialised
     * struct has to read as empty. Storing a bare index terminated by -1 is
     * the trap WM phase E paid for with a mid-gate reboot. */
    unsigned char z[UNOUI_MDI_MAX];
    unsigned char focus;
} unoui_mdi;

unoui_widget *unoui_add_mdi(unoui_window *, int x, int y, int w, int h,
                            unoui_mdi *m);
/* returns the new child's index, or -1 if the array or the z-list is full */
int  unoui_mdi_add  (unoui_mdi *, const char *title, int x, int y, int w, int h,
                     int flags, unoui_canvas *c);
void unoui_mdi_close(unoui_mdi *, int i);
void unoui_mdi_raise(unoui_mdi *, int i);      /* raise + focus */
int  unoui_mdi_focused(const unoui_mdi *);     /* child index, or -1 */
int  unoui_mdi_count  (const unoui_mdi *);     /* live children */
/* z-order walk, back to front: k = 0..count-1 -> child index, or -1 */
int  unoui_mdi_zorder (const unoui_mdi *, int k);
/* absolute rect of child `i` inside container rect `r`; .w 0 = not live */
unoui_rect unoui_mdi_child_rect(unoui_rect r, const unoui_mdi *, int i);
/* the child's interior, below its title bar and inside its frame - what a
 * child canvas is handed and clipped to */
unoui_rect unoui_mdi_content_rect(const struct unoui_theme *, unoui_rect r,
                                  const unoui_mdi *, int i);
/* pull child `i` back inside the container and up to the size floor */
void unoui_mdi_clamp(unoui_rect r, unoui_mdi *, int i);
/* the child under a point, front-to-back; -1 = none */
int  unoui_mdi_at(unoui_rect r, const unoui_mdi *, int x, int y);
/* lay every live child out: an even grid, or a stepped stack */
void unoui_mdi_tile   (const struct unoui_theme *, unoui_rect r, unoui_mdi *);
void unoui_mdi_cascade(const struct unoui_theme *, unoui_rect r, unoui_mdi *);
void unoui_mdi_draw(const struct unoui_theme *, unoui_rect r, const unoui_mdi *);

/* compute a window's canonical content origin from the theme metrics. Window
 * painters AND hit-testing use this, so what you see is what you can click. */
void unoui_content_origin(const struct unoui_theme *, const unoui_window *,
                          int *ox, int *oy);

/* ---- the event model (the portability contract) -------------------------- */
typedef enum {
    UI_EV_NONE = 0,
    UI_EV_MOUSE_DOWN, UI_EV_MOUSE_UP, UI_EV_MOUSE_MOVE,
    UI_EV_KEY,        /* a virtual key went down (UI_KEY_*)                    */
    UI_EV_CHAR,       /* a printable character was typed (.ch, ASCII)         */
    UI_EV_WHEEL,      /* scroll wheel (.wheel = signed notches)               */
    UI_EV_TICK        /* a frame tick; drives caret blink                     */
} ui_event_kind;

enum { UI_MOD_SHIFT = 1, UI_MOD_CTRL = 2, UI_MOD_ALT = 4, UI_MOD_GUI = 8 };

enum {                /* virtual keys - kept above ASCII so CHAR vs KEY split */
    UI_KEY_LEFT = 0x100, UI_KEY_RIGHT, UI_KEY_UP, UI_KEY_DOWN,
    UI_KEY_HOME, UI_KEY_END, UI_KEY_PGUP, UI_KEY_PGDN,
    UI_KEY_BACKSPACE, UI_KEY_DELETE, UI_KEY_ENTER, UI_KEY_TAB, UI_KEY_ESC
};

typedef struct {
    ui_event_kind kind;
    int x, y;         /* mouse position, screen coords (MOUSE_* / WHEEL)      */
    int button;       /* 0 = left, 1 = right, ...                             */
    int key;          /* UI_KEY_* for UI_EV_KEY                               */
    int ch;           /* ASCII for UI_EV_CHAR                                 */
    int mods;         /* UI_MOD_* bitmask                                     */
    int wheel;        /* notches for UI_EV_WHEEL (+down / -up)                */
} unoui_event;

/* mouse-capture / drag modes (shared by the input + render layers) */
enum {
    UI_CAP_NONE = 0, UI_CAP_WINDOW, UI_CAP_BUTTON, UI_CAP_VTHUMB, UI_CAP_HTHUMB,
    UI_CAP_SLIDER, UI_CAP_TEXT, UI_CAP_LIST, UI_CAP_RESIZE,
    UI_CAP_LISTBAR,              /* dragging a list's inline scrollbar thumb    */
    UI_CAP_MDIDRAG,              /* moving a UI_MDI child by its title bar      */
    UI_CAP_MDISIZE               /* resizing a UI_MDI child by its grip         */
};

/* result of feeding one event: did a widget activate / change? */
typedef struct {
    int changed;      /* nonzero if `id`/`kind`/`value` are meaningful         */
    int id;           /* the widget's app id                                  */
    int kind;         /* the widget's ui_kind, or UI_ACT_CLOSE                 */
    int value;        /* new value: toggle state, slider/scroll pos, sel idx   */
} unoui_action;

/* special action kind: the title-bar close box was clicked. `value` is the
 * window's z-index; the app should close/remove that window. */
#define UI_ACT_CLOSE 9999
/* the same contract for the other two window commands: `value` is the z-index.
 * unoui raises them, it does not implement them - minimizing needs a taskbar
 * and maximizing needs a work-area policy, and both are the shell's. */
#define UI_ACT_MIN   9998
#define UI_ACT_MAX   9997
/* tabbed documents (UI_TABS with UI_TF_CLOSE / UI_TF_PLUS): `value` is the tab
 * index for TABCLOSE, and the count at the time of the click for TABNEW. Same
 * contract as the window commands - unoui reports the gesture, the app owns the
 * document set and decides what closing or opening one means. */
#define UI_ACT_TABCLOSE 9996
#define UI_ACT_TABNEW   9995
/* a UI_MDI child's close box: `value` is the child index, `id` the widget's.
 * unoui does NOT remove the child - call unoui_mdi_close() if that is what the
 * app means by closing one, exactly as UI_ACT_CLOSE leaves the window alone. */
#define UI_ACT_MDICLOSE 9994

/* ---- the UI context (windows + interaction state) ------------------------ */
/* 24 = the pc64 shell's worst case (taskbar + desktop + Start menu + calendar
 * + all 16 apps open at once) with headroom; was 8, which the shell could hit
 * silently with ~5 app windows open. */
#define UNOUI_MAX_WINDOWS 24

typedef struct unoui_ui {
    const struct unoui_theme *theme;
    unoui_window *win[UNOUI_MAX_WINDOWS];   /* [0]=back .. [nwin-1]=front      */
    int nwin, screen_w, screen_h;

    int focus_win, focus_wi;     /* focused widget (-1 = none)                */
    int hot_win,   hot_wi;       /* hovered widget                            */
    int cap_win,   cap_wi, cap_mode;   /* mouse-captured drag target          */
    int grab_dx, grab_dy;        /* pointer offset within the grabbed thing   */
    int resize_axes;             /* UI_CAP_RESIZE: bit0 = width, bit1 = height */
    int mx, my, mdown;

    /* an open popup (menubar menu or dropdown list) */
    int popup_win, popup_wi;     /* owner widget (-1 = none)                  */
    int popup_menu;              /* menubar: which menu index                 */
    unoui_rect popup_r;
    const char **popup_items;
    int popup_n, popup_hot;

    unsigned ticks;              /* caret blink timebase                      */
    unoui_window *full;          /* fullscreen window (NULL = normal desktop) */

    /* outline drag: while a title bar is dragged, only a rubber-band outline
     * moves (drag_active); the window commits to it on release. Keeps drags
     * flicker-free - the static desktop isn't rewritten every frame. */
    int drag_active, drag_x, drag_y, drag_w, drag_h;

    /* --- appended; every field zero must reproduce the behaviour above ----- *
     * work: the area windows are clamped, maximized and snapped into - the
     *   screen minus whatever chrome the platform reserves (pc64: the taskbar).
     *   unoui_ui_init sets it to the whole screen, and a zero-sized work rect
     *   is read as "whole screen" too, so a context built by other means keeps
     *   working.
     * live_drag: 1 = UI_CAP_WINDOW moves win->r itself each event instead of
     *   the rubber band. Opt-in, because an opaque drag needs the platform to
     *   be able to repaint one window cheaply (pc64 snapshots the scene).
     * snap_preview: the UI_SNAP_* zone a live drag would commit to (phase C).
     * last_press_*: the double-click detector's previous press. */
    unoui_rect work;
    int        live_drag;
    int        snap_preview;
    unsigned   last_press_ticks;
    int        last_press_x, last_press_y;
    /* the animation context geometry tweens run in; NULL = no animation. Set
     * through unoui_wmanim_install(), never by hand - the hooks below and this
     * field are only meaningful together. */
    struct unoui_anim *anim;
} unoui_ui;

/* ui->work, or the whole screen when it was never set (w or h <= 0) */
unoui_rect unoui_work_area(const unoui_ui *);
/* Pull a window back until it is REACHABLE in the work area: at least 48 px of
 * width and 16 px of height, i.e. enough title bar to grab. Deliberately not
 * "fully inside" - a window the user parked half off the edge should stay
 * there. This is the rule a drag commits to, exposed so a platform restoring a
 * saved position applies exactly the same one. */
void unoui_clamp_window(unoui_ui *, unoui_window *);
/* the rect a snap state occupies inside ui->work (pure geometry, no state) */
unoui_rect unoui_snap_rect(const unoui_ui *, int snap);
/* enter/leave a snap state: saves restore_r on the way in, gives it back on
 * UI_SNAP_NONE, reflows the widgets either way. A window without
 * UI_WIN_RESIZE is only MOVED (centred in the target) and keeps snap NONE -
 * a fixed pixel layout must not be stretched. */
void unoui_snap_apply(unoui_ui *, unoui_window *win, int snap);

/* ---- animated geometry (optional) -----------------------------------------
 * unoui_snap_apply normally assigns the target rect and the window is simply
 * THERE on the next frame. Set these and it hands the target to an animator
 * instead, which walks win->r over to it. Both NULL - the default, and what
 * every port did before this existed - means every geometry change is instant,
 * so a port that links no animator is unaffected and needs no build-list edit.
 *
 * unoui_wmanim.c is the shipped animator (it needs unoui_anim.c); a platform
 * installs the pair with unoui_wmanim_install(). The SEAM is here rather than
 * a direct call so unoui.c gains no link dependency on the animation module.
 *
 * `anim` in the drawing hook is the animator's business; the core only routes.
 * A hook that returns 0 declines the job and the core snaps instantly, which
 * is what a full animator pool does rather than dropping the change. */
typedef int  (*unoui_geom_fn)(unoui_ui *, unoui_window *, unoui_rect target, int ms);
typedef void (*unoui_geom_tick_fn)(unoui_ui *);
extern unoui_geom_fn      unoui_geom_anim;   /* start moving win -> target     */
extern unoui_geom_tick_fn unoui_geom_tick;   /* per-frame; called from render  */
extern int unoui_snap_ms;                    /* snap duration, ms; 0 = instant */

/* absolute screen rect of a widget (menubar spans the content top edge) */
unoui_rect unoui_widget_rect(const struct unoui_theme *, const unoui_window *,
                             const unoui_widget *);

/* ---- layout audit ---------------------------------------------------------
 * Widgets are clipped to their window's content rect, so a layout that does
 * not fit is not a mess on the desktop - it is silently CUT OFF at the frame,
 * and the machine looks fine while a button reads "Allow se...". These walk a
 * window (or every window) as built and report what will be cut, measured
 * against the live font, so a whole OS's worth of screens can be swept in one
 * pass instead of squinted at one screenshot at a time.
 *
 * `cb` gets the widget index, a short reason, the widget's window-relative
 * rect and the content box it had to fit in. Returns the number of reports.
 * Lists, textareas, canvases and MDI panes are meant to hold more than they
 * show, so only their rects are checked, never their contents. */
typedef void (*unoui_audit_fn)(void *ctx, const unoui_window *win, int wi,
                               const char *why, unoui_rect widget,
                               int content_w, int content_h);
int unoui_window_audit(const struct unoui_theme *, const unoui_window *,
                       unoui_audit_fn cb, void *ctx);
int unoui_ui_audit(const unoui_ui *, unoui_audit_fn cb, void *ctx);

void          unoui_ui_init (unoui_ui *, const struct unoui_theme *, int sw, int sh);
void          unoui_ui_theme(unoui_ui *, const struct unoui_theme *);
int           unoui_ui_add  (unoui_ui *, unoui_window *);   /* topmost = focus;
                                            0 = window table full (not added) */
/* raise `win` to the top of its z-band (respecting UI_WIN_BOTTOM/TOP pins) and
 * give it focus. Use instead of hand-editing ui->win[]. No-op if not added. */
void          unoui_bring_to_front(unoui_ui *, unoui_window *win);
unoui_action  unoui_handle  (unoui_ui *, const unoui_event *);
void          unoui_render_ui(unoui_ui *);
/* Draw ONE window (chrome + widgets) with its correct focus/hot/pressed state,
 * exactly as unoui_render_ui would. Lets a platform repaint just the window
 * that moved over a snapshot of the rest of the scene - the live-drag path. */
void          unoui_render_window(unoui_ui *, unoui_window *win);
/* Draw ONLY a window's chrome (shadow, frame, title bar, buttons, resize grip)
 * - no widget pass. Honours the caller's fb clip. A window's widgets cannot
 * change while it is being dragged, so a platform can cache its rendered
 * pixels once and blit them per frame; what a MOVE really invalidates is the
 * translucent perimeter (drop shadow, anti-aliased corners), which is
 * composited against whatever is behind it. Clip to that perimeter and call
 * this. No-op on a UI_WIN_BARE window, which has no chrome. */
void          unoui_render_window_chrome(unoui_ui *, unoui_window *win);
/* Optional per-window draw profiler (a debug harness sets it; NULL = free).
 * Called with begin=1 before a window's chrome+widgets draw and begin=0
 * after; the fullscreen canvas path reports the same way. The toolkit has no
 * portable clock, so the hook owns all timing. */
extern void (*unoui_profile_win)(const char *title, int begin);
/* Draw only the rubber-band drag outline (no-op unless a drag is live). Lets a
 * platform snapshot the scene once and redraw just the outline per drag frame. */
void          unoui_draw_drag_outline(unoui_ui *);
/* Draw the drag-to-edge snap preview: a translucent accent wash plus a 1 px
 * frame over the rect the drag would commit to (no-op unless ui->snap_preview
 * is armed). Split out for the same reason as the outline above - the pc64
 * live-drag path restores its scene snapshot and redraws only this plus the
 * dragged window. The rect shown is exactly what unoui_snap_apply will produce
 * for the window being dragged, non-resizable move-only policy included, so
 * the preview never promises geometry the commit does not deliver. */
void          unoui_draw_snap_preview(unoui_ui *);
/* Invalidate any cached desktop background (call on theme / resolution change).
 * A no-op where the cache isn't compiled in. */
void          unoui_bg_invalidate(void);

/* Full-screen mode: `win` (its first UI_CANVAS) fills the whole screen with no
 * desktop / chrome, and all input routes to that canvas. NULL restores the
 * normal desktop. For games / 3D that want the whole panel. */
void          unoui_fullscreen(unoui_ui *, unoui_window *win);

/* ---- lower-level rendering (used by the UI + the static contact sheet) --- */
void unoui_desktop(const struct unoui_theme *theme, int screen_w, int screen_h);
void unoui_render (unoui_window *win, const struct unoui_theme *theme);  /* static */

/* ---- title-bar window controls ------------------------------------------- *
 * The minimize / maximize boxes a theme opts into via unoui_metrics.minbox /
 * .maxbox. The generic painter and the hit-test both derive their geometry
 * from here, so a click can only land where a button was drawn. The returned
 * rect has .w == 0 when the button is absent (theme opted out, or a BARE
 * window, which has no chrome at all). */
enum { UI_TB_MIN = 0, UI_TB_MAX = 1 };
unoui_rect unoui_titlebtn_rect(const struct unoui_theme *, const unoui_window *,
                               int which);
/* The close box for any titled rect, on whichever side unoui_metrics.closeright
 * puts it. Separate from the above because that refuses UI_WIN_NOCTL windows,
 * and an MDI child is a NOCTL window that still has a close box. .w == 0 when
 * the theme has no close box at all. */
unoui_rect unoui_closebox_rect(const struct unoui_theme *, unoui_rect winr);

/* Optional title-bar badge. When this hook is set, the generic control painter
 * asks it for each window's badge index and draws a small marker just inboard
 * of the min/max boxes. UI_BADGE_NONE = no badge; 0..UI_BADGE_N-1 select a hue
 * DERIVED from the theme's accent (channel rotation), so a badge re-skins with
 * the theme and stays distinguishable from its neighbours whatever the palette
 * is - which no single palette role can promise.
 *
 * unoui has no idea what a badge MEANS: the pc64 shell uses it to mark window
 * link-groups, and this one hook is the only thing the toolkit knows about
 * them. NULL (the default) = no badges anywhere, i.e. today's chrome. */
#define UI_BADGE_NONE (-1)
#define UI_BADGE_N      4
extern int (*unoui_win_badge)(const unoui_window *);

#endif /* UNOUI_H */

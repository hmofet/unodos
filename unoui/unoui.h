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
    UI_CANVAS      /* app-drawn region: the app renders into fb inside .r     */
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
} unoui_text;

void unoui_text_init(unoui_text *t, char *buf, int cap, int multiline);
void unoui_text_set (unoui_text *t, const char *s);

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
} unoui_widget;

/* Optional per-app icon artwork. When set, UI_ICON widgets are drawn by this
 * hook (given the icon's full rect, its `icon` id, label and flags) instead of
 * the theme's generic glyph - so a port can supply distinct app icons. */
typedef void (*unoui_icon_fn)(int icon, unoui_rect r, const char *label, int flags);
extern unoui_icon_fn unoui_icon_art;

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
    UI_WIN_RESIZE = 1 << 3    /* draggable bottom-right grip; fill widgets reflow */
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

enum { UI_MOD_SHIFT = 1, UI_MOD_CTRL = 2, UI_MOD_ALT = 4 };

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
    UI_CAP_SLIDER, UI_CAP_TEXT, UI_CAP_LIST, UI_CAP_RESIZE
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
} unoui_ui;

/* absolute screen rect of a widget (menubar spans the content top edge) */
unoui_rect unoui_widget_rect(const struct unoui_theme *, const unoui_window *,
                             const unoui_widget *);

void          unoui_ui_init (unoui_ui *, const struct unoui_theme *, int sw, int sh);
void          unoui_ui_theme(unoui_ui *, const struct unoui_theme *);
int           unoui_ui_add  (unoui_ui *, unoui_window *);   /* topmost = focus;
                                            0 = window table full (not added) */
/* raise `win` to the top of its z-band (respecting UI_WIN_BOTTOM/TOP pins) and
 * give it focus. Use instead of hand-editing ui->win[]. No-op if not added. */
void          unoui_bring_to_front(unoui_ui *, unoui_window *win);
unoui_action  unoui_handle  (unoui_ui *, const unoui_event *);
void          unoui_render_ui(unoui_ui *);
/* Draw only the rubber-band drag outline (no-op unless a drag is live). Lets a
 * platform snapshot the scene once and redraw just the outline per drag frame. */
void          unoui_draw_drag_outline(unoui_ui *);
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

#endif /* UNOUI_H */

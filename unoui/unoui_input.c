/* ===========================================================================
 * unoui input + interaction layer.
 *
 * THE PORTABILITY CONTRACT lives here: all behaviour is a pure function of the
 * abstract unoui_event stream. A port maps its native mouse/keyboard to
 * unoui_event and calls unoui_handle(); the result is identical on every
 * platform - window dragging, focus traversal, scrollbar/slider thumbs, menus
 * and dropdowns, and full multi-line text editing (caret, selection, mouse
 * placement + drag-select, arrow/word/line navigation). No platform code here.
 * ===========================================================================
 */
#include "unoui_theme.h"

/* ------------------------------------------------------------ context ----- */

void unoui_ui_init(unoui_ui *ui, const unoui_theme *t, int sw, int sh)
{
    int i;
    ui->theme = t; ui->nwin = 0; ui->screen_w = sw; ui->screen_h = sh;
    ui->focus_win = ui->focus_wi = -1;
    ui->hot_win = ui->hot_wi = -1;
    ui->cap_win = ui->cap_wi = -1; ui->cap_mode = UI_CAP_NONE;
    ui->grab_dx = ui->grab_dy = 0; ui->resize_axes = 0;
    ui->mx = ui->my = ui->mdown = 0;
    ui->popup_win = ui->popup_wi = -1; ui->popup_menu = -1;
    ui->popup_items = 0; ui->popup_n = 0; ui->popup_hot = -1;
    ui->ticks = 0;
    /* These two were left UNINITIALISED, which is only invisible because every
     * shipped unoui_ui is a static. A stack-local one starts with garbage, and
     * a non-NULL `full` makes handle_inner() hand every event to a fullscreen
     * canvas that does not exist and return NO_ACT - so a button never fires.
     * SPECTEST's S-UUI-07 builds exactly such a ui, which is why that contract
     * passed or failed depending on what the stack happened to hold. */
    ui->full = 0;
    ui->drag_active = ui->drag_x = ui->drag_y = ui->drag_w = ui->drag_h = 0;
    /* the work area defaults to the whole screen: a platform that reserves
     * chrome (a taskbar) narrows it, everyone else is unaffected */
    ui->work.x = ui->work.y = 0; ui->work.w = sw; ui->work.h = sh;
    ui->live_drag = 0; ui->snap_preview = 0;
    ui->last_press_ticks = 0; ui->last_press_x = ui->last_press_y = 0;
    for (i = 0; i < UNOUI_MAX_WINDOWS; i++) ui->win[i] = 0;
    unoui_bg_invalidate();
}

void unoui_ui_theme(unoui_ui *ui, const unoui_theme *t) { ui->theme = t; unoui_bg_invalidate(); }

int unoui_ui_add(unoui_ui *ui, unoui_window *win)
{
    int r, pos, i;
    if (ui->nwin >= UNOUI_MAX_WINDOWS) return 0;
    /* insert just above the last window of the same-or-lower band, so the list
     * stays sorted BOTTOM < normal < TOP and pins hold. */
    r = (win->flags & UI_WIN_BOTTOM) ? 0 : (win->flags & UI_WIN_TOP) ? 2 : 1;
    pos = ui->nwin;
    while (pos > 0) {
        const unoui_window *p = ui->win[pos - 1];
        int pr = (p->flags & UI_WIN_BOTTOM) ? 0 : (p->flags & UI_WIN_TOP) ? 2 : 1;
        if (pr <= r) break;
        pos--;
    }
    for (i = ui->nwin; i > pos; i--) ui->win[i] = ui->win[i - 1];
    ui->win[pos] = win; ui->nwin++;
    if (r == 1) { ui->focus_win = pos; ui->focus_wi = -1; }  /* focus new apps */
    else if (ui->focus_win >= pos) ui->focus_win++;          /* keep focus ptr */
    return 1;
}

/* ----------------------------------------------------------- helpers ------ */

static const unoui_action NO_ACT = { 0, 0, 0, 0 };

/* title-bar double click: two presses within this many UI_EV_TICKs and this
 * many px of each other. A port ticks per frame, so 24 is roughly 400 ms. */
#define DBLCLICK_TICKS 24
#define DBLCLICK_SLOP  4
/* Manhattan distance the pointer must travel before a drag un-snaps a snapped
 * window. Comfortably above DBLCLICK_SLOP, so a double-click on a maximized
 * title bar can never be read as the start of a drag off the snap. */
#define UNSNAP_SLOP    8

static int pt_in(unoui_rect r, int x, int y)
{ return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

/* UI_F_DISABLED greys a control out.  It used to grey it out and NOTHING ELSE:
 * every painter dimmed the text, and the input layer went on hitting, focusing
 * and firing the widget exactly as before.  A control that looks unavailable
 * and still works is worse than one that never greys, because the look is a
 * promise about what a click will do.  Both gates below honour it now. */
static int disabled(const unoui_widget *w) { return (w->flags & UI_F_DISABLED) != 0; }

static int interactive(const unoui_widget *w)
{
    if (disabled(w)) return 0;
    switch (w->kind) {
    case UI_BUTTON: case UI_CHECK: case UI_RADIO: case UI_TEXTAREA:
    case UI_VSCROLL: case UI_HSCROLL: case UI_SLIDER: case UI_SPINNER:
    case UI_DROPDOWN: case UI_TABS: case UI_MENUBAR: case UI_LIST: case UI_ICON:
    case UI_CANVAS:            /* app-drawn regions take clicks (games, taskbar) */
    case UI_MDI:               /* child frames drag, raise, resize and close   */
        return 1;
    case UI_FIELD: return w->edit != 0;
    default: return 0;
    }
}

static int focusable(const unoui_widget *w)
{
    if (disabled(w)) return 0;              /* Tab skips it too - see above */
    switch (w->kind) {
    case UI_BUTTON: case UI_CHECK: case UI_RADIO: case UI_TEXTAREA:
    case UI_SLIDER: case UI_SPINNER: case UI_DROPDOWN: case UI_TABS: case UI_LIST:
    case UI_CANVAS: case UI_MDI:
        return 1;
    case UI_FIELD: return w->edit != 0;
    default: return 0;
    }
}

/* forward an event to the focused canvas widget, if any; 1 = forwarded */
static int canvas_forward(unoui_ui *ui, const unoui_event *ev)
{
    unoui_widget *w;
    if (ui->focus_win < 0 || ui->focus_wi < 0) return 0;
    w = &ui->win[ui->focus_win]->w[ui->focus_wi];
    if (w->kind == UI_CANVAS && w->canvas && w->canvas->event) {
        w->canvas->event(w, ev, w->canvas->ctx); return 1;
    }
    return 0;
}

static int window_at(unoui_ui *ui, int x, int y)
{
    int i;
    for (i = ui->nwin - 1; i >= 0; i--)
        if (pt_in(ui->win[i]->r, x, y)) return i;
    return -1;
}

static int hit_widget(unoui_ui *ui, unoui_window *win, int x, int y)
{
    int i, found = -1;
    for (i = 0; i < win->nw; i++) {
        if (!interactive(&win->w[i])) continue;
        if (pt_in(unoui_widget_rect(ui->theme, win, &win->w[i]), x, y)) found = i;
    }
    return found;
}

/* z-band of a window: 0 = BOTTOM (desktop), 1 = normal, 2 = TOP (taskbar).
 * The window list is kept sorted by band, so all BOTTOM windows sit at low
 * indices and all TOP windows at high indices. */
static int pin_rank(const unoui_window *w)
{
    if (w->flags & UI_WIN_BOTTOM) return 0;
    if (w->flags & UI_WIN_TOP)    return 2;
    return 1;
}

/* move the window at `from` to index `to`, sliding the rest to fill the gap */
static void move_win(unoui_ui *ui, int from, int to)
{
    unoui_window *w = ui->win[from];
    int i;
    if (from < to)      for (i = from; i < to; i++) ui->win[i] = ui->win[i + 1];
    else if (from > to) for (i = from; i > to; i--) ui->win[i] = ui->win[i - 1];
    ui->win[to] = w;
}

/* raise idx to the top of its own band (just below the first higher band).
 * Returns the window's new index. */
static int raise_within_rank(unoui_ui *ui, int idx)
{
    int r = pin_rank(ui->win[idx]), dest = ui->nwin - 1;
    while (dest > idx && pin_rank(ui->win[dest]) > r) dest--;
    move_win(ui, idx, dest);
    return dest;
}

/* find a window's current index, or -1 */
static int win_index(unoui_ui *ui, const unoui_window *win)
{
    int i; for (i = 0; i < ui->nwin; i++) if (ui->win[i] == win) return i;
    return -1;
}

void unoui_bring_to_front(unoui_ui *ui, unoui_window *win)
{
    int idx = win_index(ui, win);
    if (idx < 0) return;
    ui->focus_win = raise_within_rank(ui, idx);
    ui->focus_wi = -1;
}

/* Windows are kept inside the WORK area, not the raw screen: a platform that
 * reserves chrome (pc64's taskbar) sets ui->work and its windows stop hiding
 * under the bar. A zero-sized work rect means "not set" and reads as the whole
 * screen, so a context built without unoui_ui_init behaves exactly as before. */
unoui_rect unoui_work_area(const unoui_ui *ui)
{
    unoui_rect r = ui->work;
    if (r.w <= 0 || r.h <= 0) {
        r.x = r.y = 0; r.w = ui->screen_w; r.h = ui->screen_h;
    }
    return r;
}

/* Keep at least 48 px of a window's width and 16 px of its height reachable
 * inside `wk` - enough title bar to grab it back with. */
static void clamp_to(unoui_rect wk, unoui_rect *r)
{
    if (r->x < wk.x - r->w + 48) r->x = wk.x - r->w + 48;
    if (r->x > wk.x + wk.w - 48) r->x = wk.x + wk.w - 48;
    if (r->y < wk.y) r->y = wk.y;
    if (r->y > wk.y + wk.h - 16) r->y = wk.y + wk.h - 16;
}

void unoui_clamp_window(unoui_ui *ui, unoui_window *w)
{ if (w) clamp_to(unoui_work_area(ui), &w->r); }

static void clamp_win(unoui_ui *ui, unoui_window *w)
{ unoui_clamp_window(ui, w); }

/* ---- drag-to-edge snap zones (phase C) ------------------------------------ *
 * The POINTER picks the zone, never the window rect: a window grabbed by the
 * right end of its title bar must still snap left when the pointer reaches the
 * left edge, and a window is clamped inside the work area while the pointer is
 * not, so only the pointer can actually reach an edge. Corners are tested
 * first, so the quarter zones win over the half/maximize strips they overlap. */
#define SNAP_EDGE_PX   8    /* distance from a work-area edge that arms a half */
#define SNAP_CORNER_PX 24   /* corner square that arms a quarter                */

static int snap_zone(const unoui_ui *ui, int x, int y)
{
    unoui_rect wk = unoui_work_area(ui);
    /* distances to each edge; negative = the pointer is past it (the taskbar
     * band below the work area still counts as "at the bottom"). */
    int dl = x - wk.x,            dr = (wk.x + wk.w - 1) - x;
    int dt = y - wk.y,            db = (wk.y + wk.h - 1) - y;
    int cl = dl < SNAP_CORNER_PX, cr = dr < SNAP_CORNER_PX;
    int ct = dt < SNAP_CORNER_PX, cb = db < SNAP_CORNER_PX;
    if (ct && cl) return UI_SNAP_TL;
    if (ct && cr) return UI_SNAP_TR;
    if (cb && cl) return UI_SNAP_BL;
    if (cb && cr) return UI_SNAP_BR;
    if (dt < SNAP_EDGE_PX) return UI_SNAP_MAX;
    if (dl < SNAP_EDGE_PX) return UI_SNAP_L;
    if (dr < SNAP_EDGE_PX) return UI_SNAP_R;
    return UI_SNAP_NONE;                 /* the bottom edge alone snaps nothing */
}

static void close_popup(unoui_ui *ui)
{
    ui->popup_wi = ui->popup_win = -1; ui->popup_menu = -1;
    ui->popup_items = 0; ui->popup_n = 0; ui->popup_hot = -1;
}

/* ----------------------------------------------------- text editing ------- */

static int ln_start(const unoui_text *t, int i)
{ while (i > 0 && t->buf[i - 1] != '\n') i--; return i; }
static int ln_end(const unoui_text *t, int i)
{ while (i < t->len && t->buf[i] != '\n') i++; return i; }

static int caret_up(const unoui_text *t)
{
    int ls = ln_start(t, t->caret), col = t->caret - ls, ps, plen;
    if (ls == 0) return t->caret;
    ps = ln_start(t, ls - 1); plen = (ls - 1) - ps;
    return ps + (col < plen ? col : plen);
}
static int caret_down(const unoui_text *t)
{
    int le = ln_end(t, t->caret), ls = ln_start(t, t->caret), col = t->caret - ls, ns, ne, nlen;
    if (le >= t->len) return t->caret;
    ns = le + 1; ne = ln_end(t, ns); nlen = ne - ns;
    return ns + (col < nlen ? col : nlen);
}

static void del_sel(unoui_text *t)
{
    int a, b, i;
    if (t->sel == t->caret) return;
    a = t->sel < t->caret ? t->sel : t->caret;
    b = t->sel < t->caret ? t->caret : t->sel;
    for (i = b; i <= t->len; i++) t->buf[a + (i - b)] = t->buf[i];
    t->len -= (b - a); t->caret = t->sel = a;
}
static void ins_ch(unoui_text *t, int ch)
{
    int i;
    del_sel(t);
    if (t->len >= t->cap - 1) return;
    for (i = t->len; i >= t->caret; i--) t->buf[i + 1] = t->buf[i];
    t->buf[t->caret] = (char)ch; t->len++; t->caret++; t->sel = t->caret;
}
static void backsp(unoui_text *t)
{
    int i;
    if (t->sel != t->caret) { del_sel(t); return; }
    if (t->caret <= 0) return;
    for (i = t->caret; i <= t->len; i++) t->buf[i - 1] = t->buf[i];
    t->len--; t->caret--; t->sel = t->caret;
}
static void del_fwd(unoui_text *t)
{
    int i;
    if (t->sel != t->caret) { del_sel(t); return; }
    if (t->caret >= t->len) return;
    for (i = t->caret + 1; i <= t->len; i++) t->buf[i - 1] = t->buf[i];
    t->len--; t->sel = t->caret;
}

static unoui_rect focus_inner(unoui_ui *ui)
{
    unoui_window *win = ui->win[ui->focus_win];
    unoui_widget *w = &win->w[ui->focus_wi];
    return ui_edit_text_rect(
               ui_edit_inner(unoui_widget_rect(ui->theme, win, w), ui->theme),
               w->edit);
}

static void mv(unoui_ui *ui, unoui_text *t, int nc, int extend)
{
    if (nc < 0) nc = 0;
    if (nc > t->len) nc = t->len;
    t->caret = nc;
    if (!extend) t->sel = nc;
    ui_text_reveal(focus_inner(ui), t);
}

/* ------------------------------------------------- value-from-pointer ----- */

static unoui_action change(unoui_widget *w)
{ unoui_action a; a.changed = 1; a.id = w->id; a.kind = w->kind; a.value = w->value; return a; }

static unoui_action set_vscroll(unoui_ui *ui, int y)
{
    unoui_window *win = ui->win[ui->cap_win]; unoui_widget *w = &win->w[ui->cap_wi];
    unoui_rect r = unoui_widget_rect(ui->theme, win, w);
    int bw = r.w, track = r.h - 2 * bw, v;
    v = track > 0 ? w->vmax * (y - (r.y + bw)) / track : 0;
    if (v < 0) v = 0;
    if (v > w->vmax) v = w->vmax;
    w->value = v; return change(w);
}
static unoui_action set_hscroll(unoui_ui *ui, int x)
{
    unoui_window *win = ui->win[ui->cap_win]; unoui_widget *w = &win->w[ui->cap_wi];
    unoui_rect r = unoui_widget_rect(ui->theme, win, w);
    int bw = r.h, track = r.w - 2 * bw, v;
    v = track > 0 ? w->vmax * (x - (r.x + bw)) / track : 0;
    if (v < 0) v = 0;
    if (v > w->vmax) v = w->vmax;
    w->value = v; return change(w);
}
static unoui_action set_slider(unoui_ui *ui, int x)
{
    unoui_window *win = ui->win[ui->cap_win]; unoui_widget *w = &win->w[ui->cap_wi];
    unoui_rect r = unoui_widget_rect(ui->theme, win, w);
    int span = r.w - 6 - 9, v;
    v = span > 0 ? w->vmin + (w->vmax - w->vmin) * (x - (r.x + 3)) / span : w->vmin;
    if (v < w->vmin) v = w->vmin;
    if (v > w->vmax) v = w->vmax;
    w->value = v; return change(w);
}
/* click / drag inside the ROWS of a list: select the row under the pointer.
 * The list is scrolled, so the row is relative to the widget's first visible
 * row (w->value) - the one geometry helper the painter uses. */
static unoui_action set_list(unoui_ui *ui, int y)
{
    unoui_window *win = ui->win[ui->cap_win]; unoui_widget *w = &win->w[ui->cap_wi];
    unoui_rect r = unoui_widget_rect(ui->theme, win, w);
    int idx = unoui_list_index_at(r, w->nitems, w->value, y);
    w->sel = idx; { unoui_action a = change(w); a.value = idx; return a; }
}

/* drag on a list's inline scrollbar thumb: map y to the first visible row.
 * Scrolling changes no app-visible VALUE (the selection stands), so this
 * reports NO_ACT; the shell repaints on the input event itself. */
static unoui_action set_listbar(unoui_ui *ui, int y)
{
    unoui_window *win = ui->win[ui->cap_win]; unoui_widget *w = &win->w[ui->cap_wi];
    unoui_rect r = unoui_widget_rect(ui->theme, win, w);
    int mt = unoui_list_maxtop(r, w->nitems);
    int bw = UI_LIST_BAR_W, track = r.h - 2 * bw, v;
    /* the LAST track pixel means "the last row", so a drag to the bottom of the
     * bar really does reach the end of the list (a /track map stops one row
     * short of it) */
    v = track > 1 ? mt * (y - (r.y + bw)) / (track - 1) : 0;
    if (v < 0) v = 0;
    if (v > mt) v = mt;
    w->value = v;
    { unoui_action a = NO_ACT; return a; }
}

/* drag on a window's CONTENT scrollbar: map y to a scroll offset. The thumb is
 * grabbed at its middle rather than where it was clicked, which for a bar this
 * narrow is what makes it feel like it follows the pointer. */
static void set_winbar(unoui_ui *ui, int y)
{
    unoui_window *win = ui->win[ui->cap_win];
    unoui_rect bar = unoui_win_bar(ui->theme, win);
    int track = bar.h - 2 * UI_WIN_BAR_W;
    int mx = unoui_win_scroll_max(ui->theme, win);
    if (!bar.w) return;
    if (track < 1) track = 1;
    unoui_win_scroll_to(ui->theme, win,
                        mx * (y - (bar.y + UI_WIN_BAR_W)) / track);
}

/* scroll a list by `d` rows, clamped. NO_ACT for the same reason as above. */
static unoui_action scroll_list(unoui_ui *ui, unoui_window *win,
                                unoui_widget *w, int d)
{
    unoui_rect r = unoui_widget_rect(ui->theme, win, w);
    int mt = unoui_list_maxtop(r, w->nitems), v = w->value + d;
    if (v > mt) v = mt;
    if (v < 0) v = 0;
    w->value = v;
    { unoui_action a = NO_ACT; return a; }
}

/* ------------------------------------------------------- press a widget --- */

static unoui_action press_widget(unoui_ui *ui, unoui_window *win, int hi,
                                 const unoui_event *ev)
{
    const unoui_theme *t = ui->theme;
    unoui_widget *w = &win->w[hi];
    unoui_rect r = unoui_widget_rect(t, win, w);
    ui->cap_win = ui->focus_win; ui->cap_wi = hi;

    switch (w->kind) {
    case UI_CANVAS:
        ui->focus_wi = hi; ui->cap_mode = UI_CAP_NONE;
        if (w->canvas && w->canvas->event) w->canvas->event(w, ev, w->canvas->ctx);
        return NO_ACT;

    case UI_BUTTON: case UI_CHECK: case UI_RADIO: case UI_ICON:
        ui->cap_mode = UI_CAP_BUTTON; return NO_ACT;   /* fires on release */

    case UI_MDI: {
        /* Precedence mirrors a real window's: resize zone, close box, then
         * title-bar drag, then the body. The press always raises and focuses
         * first, so every later test reads the child the user just brought
         * forward. Geometry comes from unoui_mdi_child_rect, the same call the
         * painter uses. */
        unoui_mdi *m = w->mdi;
        unoui_rect q;
        int ci, fw = t->m.frame_w, th = t->m.title_h;
        ui->focus_wi = hi; ui->cap_mode = UI_CAP_NONE;
        if (!m) return NO_ACT;
        ci = unoui_mdi_at(r, m, ev->x, ev->y);
        if (ci < 0) return NO_ACT;
        unoui_mdi_raise(m, ci);
        q = unoui_mdi_child_rect(r, m, ci);

        if (m->ch[ci].flags & UI_MDI_RESIZE) {
            int gs = 14, eg = fw + 3;
            int in_r = ev->x >= q.x + q.w - eg && ev->x < q.x + q.w;
            int in_b = ev->y >= q.y + q.h - eg && ev->y < q.y + q.h;
            int in_c = ev->x >= q.x + q.w - gs && ev->y >= q.y + q.h - gs &&
                       ev->x < q.x + q.w && ev->y < q.y + q.h;
            if (in_c || (ev->y >= q.y + th && (in_r || in_b))) {
                ui->cap_mode = UI_CAP_MDISIZE;
                ui->grab_dx = ev->x - (q.x + q.w);
                ui->grab_dy = ev->y - (q.y + q.h);
                ui->resize_axes = in_c ? 3 : ((in_r ? 1 : 0) | (in_b ? 2 : 0));
                { unoui_action a = change(w); a.value = ci; return a; }
            }
        }
        if (ev->y >= q.y && ev->y < q.y + th) {
            if (t->m.closebox) {
                unoui_rect cb = unoui_closebox_rect(t, q);
                if (pt_in(cb, ev->x, ev->y)) {
                    unoui_action a = change(w);
                    a.kind = UI_ACT_MDICLOSE; a.value = ci; return a;
                }
            }
            ui->cap_mode = UI_CAP_MDIDRAG;
            ui->grab_dx = ev->x - q.x;
            ui->grab_dy = ev->y - q.y;
            { unoui_action a = change(w); a.value = ci; return a; }
        }
        /* the body: the child's own canvas, if it has one */
        if (m->ch[ci].canvas && m->ch[ci].canvas->event)
            m->ch[ci].canvas->event(0, ev, m->ch[ci].canvas->ctx);
        { unoui_action a = change(w); a.value = ci; return a; }
    }

    case UI_FIELD: case UI_TEXTAREA: {
        unoui_rect box = ui_edit_inner(r, t);
        unoui_rect eye = ui_edit_eye_rect(box, w->edit);
        unoui_rect in;
        /* the eye first: a click there toggles the mask instead of moving the
         * caret, and must NOT start a text drag */
        if (eye.w && pt_in(eye, ev->x, ev->y)) {
            w->edit->revealed = !w->edit->revealed;
            ui->cap_mode = UI_CAP_NONE;
            return NO_ACT;
        }
        in = ui_edit_text_rect(box, w->edit);
        w->edit->caret = ui_text_index_at(in, w->edit, ev->x, ev->y);
        w->edit->sel = w->edit->caret;
        ui->cap_mode = UI_CAP_TEXT; ui_text_reveal(in, w->edit);
        return NO_ACT;
    }
    case UI_VSCROLL: {
        int bw = r.w, step = w->vmax / 10 + 1;
        if (ev->y < r.y + bw)            { w->value -= step; }
        else if (ev->y > r.y + r.h - bw) { w->value += step; }
        else { ui->cap_mode = UI_CAP_VTHUMB; return set_vscroll(ui, ev->y); }
        if (w->value < 0) w->value = 0;
        if (w->value > w->vmax) w->value = w->vmax;
        ui->cap_mode = UI_CAP_NONE; return change(w);
    }
    case UI_HSCROLL: {
        int bw = r.h, step = w->vmax / 10 + 1;
        if (ev->x < r.x + bw)            { w->value -= step; }
        else if (ev->x > r.x + r.w - bw) { w->value += step; }
        else { ui->cap_mode = UI_CAP_HTHUMB; return set_hscroll(ui, ev->x); }
        if (w->value < 0) w->value = 0;
        if (w->value > w->vmax) w->value = w->vmax;
        ui->cap_mode = UI_CAP_NONE; return change(w);
    }
    case UI_SLIDER:
        ui->cap_mode = UI_CAP_SLIDER; return set_slider(ui, ev->x);

    case UI_SPINNER:
        ui->cap_mode = UI_CAP_NONE;
        if (ev->x >= r.x + r.w - 12) {
            if (ev->y < r.y + r.h / 2) w->value++; else w->value--;
            if (w->value < w->vmin) w->value = w->vmin;
            if (w->value > w->vmax) w->value = w->vmax;
            return change(w);
        }
        return NO_ACT;

    case UI_LIST: {
        unoui_rect bar = unoui_list_bar(r, w->nitems);
        if (bar.w && ev->x >= bar.x) {           /* the inline scrollbar */
            if (ev->y < bar.y + UI_LIST_BAR_W) {          /* up arrow   */
                ui->cap_mode = UI_CAP_NONE; return scroll_list(ui, win, w, -1);
            }
            if (ev->y > bar.y + bar.h - UI_LIST_BAR_W) {  /* down arrow */
                ui->cap_mode = UI_CAP_NONE; return scroll_list(ui, win, w, 1);
            }
            ui->cap_mode = UI_CAP_LISTBAR; return set_listbar(ui, ev->y);
        }
        ui->cap_mode = UI_CAP_LIST; return set_list(ui, ev->y);
    }

    case UI_TABS: {
        /* one geometry for the painter and the click - see unoui_tabs_hit() */
        unoui_tabs_model tm;
        int k = -1, part;
        ui->cap_mode = UI_CAP_NONE;
        unoui_tabs_model_of(w, &tm);
        part = unoui_tabs_hit(t, r, &tm, ev->x, ev->y, &k);
        if (part == UI_TAB_SEL && k >= 0) {
            w->sel = k; { unoui_action a = change(w); a.value = k; return a; }
        }
        if (part == UI_TAB_CLOSE && k >= 0) {
            unoui_action a = change(w); a.kind = UI_ACT_TABCLOSE; a.value = k; return a;
        }
        if (part == UI_TAB_PLUS) {
            unoui_action a = change(w); a.kind = UI_ACT_TABNEW; a.value = w->nitems;
            return a;
        }
        if (part == UI_TAB_OVER) {          /* scroll one tab further along */
            if (w->value < unoui_tabs_maxfirst(t, r, &tm)) w->value++;
            return change(w);
        }
        return NO_ACT;
    }
    case UI_DROPDOWN:
        ui->cap_mode = UI_CAP_NONE;
        ui->popup_win = ui->focus_win; ui->popup_wi = hi; ui->popup_menu = -1;
        ui->popup_items = w->items; ui->popup_n = w->nitems; ui->popup_hot = w->sel;
        { unoui_rect pr = { r.x, r.y + r.h, r.w, w->nitems * ui_prow_h() + 4 }; ui->popup_r = pr; }
        return NO_ACT;

    case UI_MENUBAR: {
        int tx, mi = unoui_menubar_index_at(t, r, w->menus, w->nmenus, ev->x, &tx);
        ui->cap_mode = UI_CAP_NONE;
        if (mi >= 0) {
            const unoui_menu *m = &w->menus[mi];
            int k, mw = 0;
            for (k = 0; k < m->nitems; k++) {
                int ww = fb_text_w(m->items[k]); if (ww > mw) mw = ww;
            }
            mw += 16;
            ui->popup_win = ui->focus_win; ui->popup_wi = hi; ui->popup_menu = mi;
            ui->popup_items = m->items; ui->popup_n = m->nitems; ui->popup_hot = -1;
            { unoui_rect pr = { tx, r.y + r.h, mw, m->nitems * ui_prow_h() + 4 }; ui->popup_r = pr; }
        }
        return NO_ACT;
    }
    default:
        ui->cap_mode = UI_CAP_NONE; return NO_ACT;
    }
}

static void set_radio(unoui_window *win, int i)
{
    int j;
    for (j = i; j >= 0 && win->w[j].kind == UI_RADIO; j--) win->w[j].flags &= ~UI_F_CHECKED;
    for (j = i + 1; j < win->nw && win->w[j].kind == UI_RADIO; j++) win->w[j].flags &= ~UI_F_CHECKED;
    win->w[i].flags |= UI_F_CHECKED;
}

static unoui_action activate(unoui_ui *ui, int wn, int wi)
{
    unoui_window *win = ui->win[wn]; unoui_widget *w = &win->w[wi];
    unoui_action a; a.changed = 1; a.id = w->id; a.kind = w->kind; a.value = 1;
    switch (w->kind) {
    case UI_CHECK: w->flags ^= UI_F_CHECKED; a.value = (w->flags & UI_F_CHECKED) ? 1 : 0; break;
    case UI_RADIO: set_radio(win, wi); a.value = 1; break;
    default: break;
    }
    return a;
}

static unoui_action popup_commit(unoui_ui *ui, int idx)
{
    unoui_window *win = ui->win[ui->popup_win]; unoui_widget *w = &win->w[ui->popup_wi];
    unoui_action a; a.changed = 1; a.id = w->id; a.kind = w->kind;
    if (w->kind == UI_DROPDOWN) { w->sel = idx; a.value = idx; }
    else /* menubar */         { a.value = ui->popup_menu * 256 + idx; }
    return a;
}

/* -------------------------------------------------------- focus + keys ---- */

static void focus_step(unoui_ui *ui, int dir)
{
    unoui_window *win;
    int n, start, i, j;
    if (ui->nwin == 0) return;
    /* Tab traverses the FOCUSED window - not blindly the front one, which with
     * a pinned taskbar (UI_WIN_TOP) is always the bar, never the app. */
    if (ui->focus_win < 0 || ui->focus_win >= ui->nwin) ui->focus_win = ui->nwin - 1;
    win = ui->win[ui->focus_win];
    n = win->nw; if (n == 0) return;
    start = ui->focus_wi;
    for (i = 1; i <= n; i++) {
        j = (start < 0 ? (dir > 0 ? -1 : 0) : start) + dir * i;
        j %= n; if (j < 0) j += n;
        if (focusable(&win->w[j])) {
            ui->focus_wi = j;
            if (win->w[j].edit) win->w[j].edit->sel = win->w[j].edit->caret;
            return;
        }
    }
}

static unoui_action key_event(unoui_ui *ui, const unoui_event *ev)
{
    unoui_window *win;
    unoui_widget *w;
    int ext = (ev->mods & UI_MOD_SHIFT) != 0;

    if (ev->key == UI_KEY_ESC) { close_popup(ui); return NO_ACT; }
    if (ev->key == UI_KEY_TAB) { focus_step(ui, ext ? -1 : 1); return NO_ACT; }
    if (ui->focus_win < 0 || ui->focus_wi < 0) return NO_ACT;
    if (canvas_forward(ui, ev)) return NO_ACT;   /* a focused canvas gets keys */

    win = ui->win[ui->focus_win]; w = &win->w[ui->focus_wi];

    if (w->edit) {                              /* text editor keys */
        unoui_text *t = w->edit;
        switch (ev->key) {
        case UI_KEY_LEFT:  mv(ui, t, t->caret - 1, ext); break;
        case UI_KEY_RIGHT: mv(ui, t, t->caret + 1, ext); break;
        case UI_KEY_UP:    if (t->multiline) mv(ui, t, caret_up(t), ext);   break;
        case UI_KEY_DOWN:  if (t->multiline) mv(ui, t, caret_down(t), ext); break;
        case UI_KEY_HOME:  mv(ui, t, ln_start(t, t->caret), ext); break;
        case UI_KEY_END:   mv(ui, t, ln_end(t, t->caret), ext);   break;
        case UI_KEY_BACKSPACE: backsp(t); ui_text_reveal(focus_inner(ui), t); break;
        case UI_KEY_DELETE:    del_fwd(t); ui_text_reveal(focus_inner(ui), t); break;
        case UI_KEY_ENTER:
            if (t->multiline) { ins_ch(t, '\n'); ui_text_reveal(focus_inner(ui), t); }
            else { unoui_action a; a.changed = 1; a.id = w->id; a.kind = w->kind; a.value = 0; return a; }
            break;
        default: break;
        }
        return NO_ACT;
    }

    switch (w->kind) {                          /* non-text focus navigation */
    case UI_BUTTON: case UI_CHECK: case UI_RADIO:
        if (ev->key == UI_KEY_ENTER) return activate(ui, ui->focus_win, ui->focus_wi);
        break;
    case UI_SLIDER: case UI_SPINNER:
        if (ev->key == UI_KEY_LEFT || ev->key == UI_KEY_DOWN) {
            if (w->value > w->vmin) w->value--;
            return change(w);
        }
        if (ev->key == UI_KEY_RIGHT || ev->key == UI_KEY_UP) {
            if (w->value < w->vmax) w->value++;
            return change(w);
        }
        break;
    case UI_TABS: {
        /* an overflowing strip scrolls the new selection into view, for the
         * same reason a list does - otherwise the keys select what you cannot see */
        unoui_tabs_model tm;
        unoui_rect r;
        int ns = w->sel;
        if (ev->key == UI_KEY_LEFT  && ns > 0)              ns--;
        else if (ev->key == UI_KEY_RIGHT && ns < w->nitems - 1) ns++;
        else break;
        w->sel = ns;
        r = unoui_widget_rect(ui->theme, ui->win[ui->focus_win], w);
        unoui_tabs_model_of(w, &tm);
        w->value = unoui_tabs_reveal(ui->theme, r, &tm, ns);
        { unoui_action a = change(w); a.value = ns; return a; }
    }
    case UI_LIST: {
        /* the list scrolls, so every key that moves the selection also pulls it
         * back into view; PgUp/PgDn step a screenful, Home/End go to the ends. */
        unoui_rect r = unoui_widget_rect(ui->theme, ui->win[ui->focus_win], w);
        int rows = unoui_list_rows(r), ns = w->sel;
        switch (ev->key) {
        case UI_KEY_UP:   ns--;         break;
        case UI_KEY_DOWN: ns++;         break;
        case UI_KEY_PGUP: ns -= rows;   break;
        case UI_KEY_PGDN: ns += rows;   break;
        case UI_KEY_HOME: ns = 0;       break;
        case UI_KEY_END:  ns = w->nitems - 1; break;
        default: return NO_ACT;
        }
        if (ns < 0) ns = 0;
        if (ns > w->nitems - 1) ns = w->nitems - 1;
        if (ns == w->sel) return NO_ACT;
        w->sel = ns;
        w->value = unoui_list_reveal(r, w->nitems, ns, w->value);
        { unoui_action a = change(w); a.value = ns; return a; }
    }
    case UI_DROPDOWN:
        if (ev->key == UI_KEY_UP && w->sel > 0)              { w->sel--; { unoui_action a = change(w); a.value = w->sel; return a; } }
        if (ev->key == UI_KEY_DOWN && w->sel < w->nitems-1)  { w->sel++; { unoui_action a = change(w); a.value = w->sel; return a; } }
        break;
    default: break;
    }
    return NO_ACT;
}

/* ----------------------------------------------------------- dispatch ----- */

static unoui_action handle_inner(unoui_ui *ui, const unoui_event *ev);

/* Public entry: if the focused window uses a per-window font (the Editor's doc
 * font), push it so text hit-testing (caret placement, reveal) measures with
 * the SAME face the renderer drew - otherwise a click lands on the wrong glyph. */
unoui_action unoui_handle(unoui_ui *ui, const unoui_event *ev)
{
    unoui_window *fw = (ui->focus_win >= 0 && ui->focus_win < ui->nwin)
                       ? ui->win[ui->focus_win] : 0;
    int pushed = (fw && fw->font_slot != UI_FONT_INHERIT && unoui_font_push != 0);
    unoui_action a;
    if (pushed) unoui_font_push(fw->font_slot);
    a = handle_inner(ui, ev);
    if (pushed) unoui_font_pop();
    return a;
}

static unoui_action handle_inner(unoui_ui *ui, const unoui_event *ev)
{
    /* fullscreen: the window's canvas owns all input (TICK still blinks) */
    if (ui->full) {
        int i;
        if (ev->kind == UI_EV_TICK) ui->ticks++;
        for (i = 0; i < ui->full->nw; i++)
            if (ui->full->w[i].kind == UI_CANVAS) {
                unoui_widget *w = &ui->full->w[i];
                if (w->canvas && w->canvas->event) w->canvas->event(w, ev, w->canvas->ctx);
                break;
            }
        return NO_ACT;
    }

    switch (ev->kind) {

    case UI_EV_TICK:
        ui->ticks++;
        return NO_ACT;

    case UI_EV_CHAR: {
        unoui_window *win; unoui_widget *w;
        if (canvas_forward(ui, ev)) return NO_ACT;
        if (ui->focus_win < 0 || ui->focus_wi < 0) return NO_ACT;
        win = ui->win[ui->focus_win]; w = &win->w[ui->focus_wi];
        if (w->edit && ev->ch >= 32 && ev->ch < 127) {
            ins_ch(w->edit, ev->ch);
            ui_text_reveal(focus_inner(ui), w->edit);
        }
        return NO_ACT;
    }

    case UI_EV_KEY:
        return key_event(ui, ev);

    case UI_EV_WHEEL: {                          /* scroll the hovered scrollbar/list */
        if (ui->hot_win >= 0 && ui->hot_wi >= 0) {
            unoui_widget *w = &ui->win[ui->hot_win]->w[ui->hot_wi];
            if (w->kind == UI_CANVAS && w->canvas && w->canvas->event) {
                w->canvas->event(w, ev, w->canvas->ctx);   /* app-scrolled region */
                return NO_ACT;
            }
            if (w->kind == UI_VSCROLL || w->kind == UI_HSCROLL) {
                w->value += ev->wheel * (w->vmax / 10 + 1);
                if (w->value < 0) w->value = 0;
                if (w->value > w->vmax) w->value = w->vmax;
                return change(w);
            }
            if (w->kind == UI_LIST)                  /* 3 rows a notch */
                return scroll_list(ui, ui->win[ui->hot_win], w, ev->wheel * 3);
        }
        /* nothing under the pointer wanted it: if the window itself scrolls,
         * the wheel scrolls the window. Anywhere over it, not just on the bar -
         * a scroll gesture that only works on a 12 px strip is a scroll gesture
         * nobody finds. */
        { int wn = window_at(ui, ui->mx, ui->my);
          if (wn >= 0 && (ui->win[wn]->flags & UI_WIN_VSCROLL)) {
              unoui_window *win = ui->win[wn];
              int step = 3 * ui_row_h();
              unoui_win_scroll_to(ui->theme, win, win->scroll_y + ev->wheel * step);
              return NO_ACT;
          } }
        return NO_ACT;
    }

    case UI_EV_MOUSE_MOVE: {
        ui->mx = ev->x; ui->my = ev->y;
        switch (ui->cap_mode) {
        case UI_CAP_WINDOW: {
            unoui_window *win = ui->win[ui->cap_win];
            unoui_rect wk = unoui_work_area(ui);
            if (ui->live_drag) {
                /* opaque drag: the window itself follows the pointer, so the
                 * platform repaints it (unoui_render_window) over a snapshot
                 * of the rest of the scene. drag_active stays 0 - there is no
                 * rubber band to draw and nothing to commit on release. */
                if (win->snap != UI_SNAP_NONE) {
                    /* Un-snap on the first real MOTION, never on the press: the
                     * first half of a double-click is a press on the title bar
                     * of a maximized window, and un-snapping there would leave
                     * the second half re-maximizing what it meant to restore.
                     * Past the slop the pre-snap SIZE comes back immediately -
                     * what you drag is what you drop - and the grab keeps its
                     * relative position along the title bar (a press 80 % of
                     * the way across a maximized bar stays 80 % across the
                     * restored one) so the window does not jump. */
                    int mx = ev->x - ui->last_press_x, my = ev->y - ui->last_press_y;
                    if (mx < 0) mx = -mx;
                    if (my < 0) my = -my;
                    if (mx + my < UNSNAP_SLOP) return NO_ACT;
                    if (win->restore_r.w > 0) {
                        int rel = (win->r.w > 0) ? (ui->grab_dx * 1024) / win->r.w : 0;
                        win->r.w = win->restore_r.w; win->r.h = win->restore_r.h;
                        ui->grab_dx = (rel * win->r.w) / 1024;
                        if (ui->grab_dy > win->r.h - 1) ui->grab_dy = win->r.h - 1;
                    }
                    win->snap = UI_SNAP_NONE;
                    unoui_reflow_window(ui->theme, win);
                }
                win->r.x = ev->x - ui->grab_dx; win->r.y = ev->y - ui->grab_dy;
                clamp_to(wk, &win->r);
                /* arm (or disarm) the snap target the release would commit to.
                 * Purely advisory until mouse-up: nothing about the window
                 * changes while a zone is armed, so dragging back out of one
                 * costs nothing to undo. */
                ui->snap_preview = snap_zone(ui, ev->x, ev->y);
                return NO_ACT;
            }
            /* move only the outline; the window commits on release */
            { unoui_rect d;
              d.x = ev->x - ui->grab_dx; d.y = ev->y - ui->grab_dy;
              d.w = ui->drag_w; d.h = ui->drag_h;
              clamp_to(wk, &d);
              ui->drag_x = d.x; ui->drag_y = d.y; }
            return NO_ACT;
        }
        case UI_CAP_RESIZE: {
            unoui_window *win = ui->win[ui->cap_win];
            int nw = (ev->x - ui->grab_dx) - win->r.x;
            int nh = (ev->y - ui->grab_dy) - win->r.y;
            if (!(ui->resize_axes & 1)) nw = win->r.w;   /* edge grab: one axis */
            if (!(ui->resize_axes & 2)) nh = win->r.h;
            if (nw < win->min_w) nw = win->min_w;
            if (nh < win->min_h) nh = win->min_h;
            if (win->r.x + nw > ui->screen_w) nw = ui->screen_w - win->r.x;
            if (win->r.y + nh > ui->screen_h) nh = ui->screen_h - win->r.y;
            win->r.w = nw; win->r.h = nh;
            unoui_reflow_window(ui->theme, win);
            return NO_ACT;
        }
        case UI_CAP_MDIDRAG: case UI_CAP_MDISIZE: {
            /* Both act on the FOCUSED child, because the press that started the
             * capture raised it. Child geometry is relative to the container,
             * so the container rect has to be re-derived each move - it travels
             * with its window. unoui_mdi_clamp then enforces the size floor and
             * keeps the child inside the box, which is the whole containment
             * guarantee in one call. */
            unoui_window *win = ui->win[ui->cap_win];
            unoui_widget *w = &win->w[ui->cap_wi];
            unoui_mdi *m = w->mdi;
            unoui_rect r;
            int ci;
            if (!m) return NO_ACT;
            r = unoui_widget_rect(ui->theme, win, w);
            ci = unoui_mdi_focused(m);
            if (ci < 0) return NO_ACT;
            if (ui->cap_mode == UI_CAP_MDIDRAG) {
                m->ch[ci].r.x = (ev->x - ui->grab_dx) - r.x;
                m->ch[ci].r.y = (ev->y - ui->grab_dy) - r.y;
            } else {
                unoui_rect q = unoui_mdi_child_rect(r, m, ci);
                int nw = (ev->x - ui->grab_dx) - q.x;
                int nh = (ev->y - ui->grab_dy) - q.y;
                if (!(ui->resize_axes & 1)) nw = q.w;   /* edge grab: one axis */
                if (!(ui->resize_axes & 2)) nh = q.h;
                m->ch[ci].r.w = nw; m->ch[ci].r.h = nh;
            }
            unoui_mdi_clamp(r, m, ci);
            return NO_ACT;
        }
        case UI_CAP_VTHUMB: return set_vscroll(ui, ev->y);
        case UI_CAP_HTHUMB: return set_hscroll(ui, ev->x);
        case UI_CAP_SLIDER: return set_slider(ui, ev->x);
        case UI_CAP_LIST:   return set_list(ui, ev->y);
        case UI_CAP_LISTBAR: return set_listbar(ui, ev->y);
        case UI_CAP_WINBAR:  set_winbar(ui, ev->y); { unoui_action a = NO_ACT; return a; }
        case UI_CAP_TEXT: {
            unoui_window *win = ui->win[ui->cap_win];
            unoui_widget *w = &win->w[ui->cap_wi];
            unoui_rect in = ui_edit_text_rect(
                ui_edit_inner(unoui_widget_rect(ui->theme, win, w), ui->theme),
                w->edit);
            w->edit->caret = ui_text_index_at(in, w->edit, ev->x, ev->y);
            ui_text_reveal(in, w->edit); return NO_ACT;
        }
        default: break;
        }
        /* while the button is held on a focused canvas, forward drag moves */
        if (ui->mdown && canvas_forward(ui, ev)) return NO_ACT;
        /* plain hover tracking */
        { int wn = window_at(ui, ev->x, ev->y);
          ui->hot_win = wn;
          ui->hot_wi = (wn >= 0) ? hit_widget(ui, ui->win[wn], ev->x, ev->y) : -1; }
        if (ui->popup_wi >= 0) {
            int idx = (ev->y - (ui->popup_r.y + 2)) / ui_prow_h();
            ui->popup_hot = (idx >= 0 && idx < ui->popup_n) ? idx : -1;
        }
        return NO_ACT;
    }

    case UI_EV_MOUSE_DOWN: {
        unoui_window *win; int wn, hi;
        ui->mx = ev->x; ui->my = ev->y; ui->mdown = 1;

        if (ui->popup_wi >= 0) {                 /* a popup is open */
            if (pt_in(ui->popup_r, ev->x, ev->y)) {
                int idx = (ev->y - (ui->popup_r.y + 2)) / ui_prow_h();
                if (idx < 0) idx = 0;
                if (idx >= ui->popup_n) idx = ui->popup_n - 1;
                { unoui_action a = popup_commit(ui, idx); close_popup(ui); return a; }
            }
            close_popup(ui); return NO_ACT;
        }

        wn = window_at(ui, ev->x, ev->y);
        if (wn < 0) { ui->focus_wi = -1; return NO_ACT; }
        ui->focus_win = raise_within_rank(ui, wn);   /* pin-aware; keeps bars */
        win = ui->win[ui->focus_win];

        if ((win->flags & UI_WIN_RESIZE) && !(win->flags & UI_WIN_BARE)) {
            /* resize zones: a generous bottom-right corner, plus the right and
             * bottom window edges (frame + a few px inside) - so grabbing any
             * edge works like a normal desktop, not just an invisible corner. */
            int gs = 18, eg = ui->theme->m.frame_w + 3;
            int in_r = ev->x >= win->r.x + win->r.w - eg && ev->x < win->r.x + win->r.w;
            int in_b = ev->y >= win->r.y + win->r.h - eg && ev->y < win->r.y + win->r.h;
            int in_c = ev->x >= win->r.x + win->r.w - gs && ev->y >= win->r.y + win->r.h - gs &&
                       ev->x < win->r.x + win->r.w && ev->y < win->r.y + win->r.h;
            int below_title = ev->y >= win->r.y + ui->theme->m.title_h;
            if (in_c || (below_title && (in_r || in_b))) {
                ui->cap_mode = UI_CAP_RESIZE; ui->cap_win = ui->focus_win;
                ui->grab_dx = ev->x - (win->r.x + win->r.w);
                ui->grab_dy = ev->y - (win->r.y + win->r.h);
                /* edge grabs resize only that axis: pin the other by parking
                 * its grab offset so the pointer tracks the same edge. */
                ui->resize_axes = in_c ? 3 : ((in_r ? 1 : 0) | (in_b ? 2 : 0));
                return NO_ACT;
            }
        }

        if (!(win->flags & UI_WIN_BARE) &&           /* bare windows don't drag */
            ev->y >= win->r.y && ev->y < win->r.y + ui->theme->m.title_h) {
            const unoui_theme *t = ui->theme;
            if (t->m.closebox) {                     /* title-bar close box */
                unoui_rect cb = unoui_closebox_rect(t, win->r);
                if (pt_in(cb, ev->x, ev->y)) {
                    unoui_action a; a.changed = 1; a.id = 0;
                    a.kind = UI_ACT_CLOSE; a.value = ui->focus_win; return a;
                }
            }
            {   /* minimize / maximize boxes. Precedence is close, min, max,
                 * then drag; an absent button has a zero-width rect, so a
                 * theme that opted out can never be hit. The maximize box
                 * still reports on a non-resizable window - it is drawn
                 * disabled and the app ignores the action - so the click is
                 * swallowed instead of starting a drag from the button. */
                unoui_rect mb = unoui_titlebtn_rect(t, win, UI_TB_MIN);
                unoui_rect xb = unoui_titlebtn_rect(t, win, UI_TB_MAX);
                unoui_action a; a.changed = 1; a.id = 0; a.value = ui->focus_win;
                if (pt_in(mb, ev->x, ev->y)) { a.kind = UI_ACT_MIN; return a; }
                if (pt_in(xb, ev->x, ev->y)) { a.kind = UI_ACT_MAX; return a; }
            }
            /* Double-click the title bar = maximize/restore. ui->ticks advances
             * on UI_EV_TICK, which a port feeds per frame (~60 Hz on pc64), so
             * 24 ticks is about 400 ms. The press is consumed: no drag starts,
             * and last_press_ticks resets so a third click is a fresh first. */
            if (ui->last_press_ticks &&
                ui->ticks - ui->last_press_ticks < DBLCLICK_TICKS &&
                ev->x - ui->last_press_x < DBLCLICK_SLOP &&
                ui->last_press_x - ev->x < DBLCLICK_SLOP &&
                ev->y - ui->last_press_y < DBLCLICK_SLOP &&
                ui->last_press_y - ev->y < DBLCLICK_SLOP) {
                unoui_action a; a.changed = 1; a.id = 0;
                a.kind = UI_ACT_MAX; a.value = ui->focus_win;
                ui->last_press_ticks = 0;        /* a third click starts over  */
                return a;
            }
            ui->last_press_ticks = ui->ticks | 1u;   /* 0 means "no previous"  */
            ui->last_press_x = ev->x; ui->last_press_y = ev->y;

            ui->snap_preview = 0;             /* a fresh drag arms nothing yet */
            ui->cap_mode = UI_CAP_WINDOW; ui->cap_win = ui->focus_win;
            ui->grab_dx = ev->x - win->r.x; ui->grab_dy = ev->y - win->r.y;
            if (!ui->live_drag) {
                ui->drag_active = 1;                 /* rubber-band outline drag */
                ui->drag_x = win->r.x; ui->drag_y = win->r.y;
                ui->drag_w = win->r.w; ui->drag_h = win->r.h;
            }
            return NO_ACT;
        }
        /* the content scrollbar, before the widgets: it is drawn OVER the
         * right-hand end of the content, so it has to be hit-tested there too */
        { unoui_rect bar = unoui_win_bar(ui->theme, win);
          if (bar.w && pt_in(bar, ev->x, ev->y)) {
              ui->cap_mode = UI_CAP_WINBAR; ui->cap_win = ui->focus_win;
              set_winbar(ui, ev->y);
              return NO_ACT;
          } }
        hi = hit_widget(ui, win, ev->x, ev->y);
        if (hi < 0) { ui->focus_wi = -1; return NO_ACT; }
        if (focusable(&win->w[hi])) ui->focus_wi = hi;
        return press_widget(ui, win, hi, ev);
    }

    case UI_EV_MOUSE_UP: {
        unoui_action a = NO_ACT;
        ui->mdown = 0;
        if (ui->cap_mode == UI_CAP_WINDOW && ui->drag_active) {  /* commit the drag */
            unoui_window *win = ui->win[ui->cap_win];
            win->r.x = ui->drag_x; win->r.y = ui->drag_y;
            clamp_win(ui, win); ui->drag_active = 0;
        }
        else if (ui->cap_mode == UI_CAP_WINDOW && ui->snap_preview) {
            /* live drag released inside a snap zone: the free position the
             * window is sitting at is discarded in favour of the zone, and
             * unoui_snap_apply banks the pre-snap rect as restore_r. */
            unoui_snap_apply(ui, ui->win[ui->cap_win], ui->snap_preview);
        }
        else if (ui->cap_mode == UI_CAP_BUTTON &&
            hit_widget(ui, ui->win[ui->cap_win], ev->x, ev->y) == ui->cap_wi)
            a = activate(ui, ui->cap_win, ui->cap_wi);
        else if (ui->cap_mode == UI_CAP_NONE) canvas_forward(ui, ev);  /* canvas release */
        ui->snap_preview = 0;     /* armed or not, the preview dies with the drag */
        ui->cap_mode = UI_CAP_NONE;
        return a;
    }

    default:
        return NO_ACT;
    }
}

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
    ui->grab_dx = ui->grab_dy = 0;
    ui->mx = ui->my = ui->mdown = 0;
    ui->popup_win = ui->popup_wi = -1; ui->popup_menu = -1;
    ui->popup_items = 0; ui->popup_n = 0; ui->popup_hot = -1;
    ui->ticks = 0;
    for (i = 0; i < UNOUI_MAX_WINDOWS; i++) ui->win[i] = 0;
}

void unoui_ui_theme(unoui_ui *ui, const unoui_theme *t) { ui->theme = t; }

void unoui_ui_add(unoui_ui *ui, unoui_window *win)
{
    int r, pos, i;
    if (ui->nwin >= UNOUI_MAX_WINDOWS) return;
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
}

/* ----------------------------------------------------------- helpers ------ */

static const unoui_action NO_ACT = { 0, 0, 0, 0 };

static int pt_in(unoui_rect r, int x, int y)
{ return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static int interactive(const unoui_widget *w)
{
    switch (w->kind) {
    case UI_BUTTON: case UI_CHECK: case UI_RADIO: case UI_TEXTAREA:
    case UI_VSCROLL: case UI_HSCROLL: case UI_SLIDER: case UI_SPINNER:
    case UI_DROPDOWN: case UI_TABS: case UI_MENUBAR: case UI_LIST: case UI_ICON:
    case UI_CANVAS:            /* app-drawn regions take clicks (games, taskbar) */
        return 1;
    case UI_FIELD: return w->edit != 0;
    default: return 0;
    }
}

static int focusable(const unoui_widget *w)
{
    switch (w->kind) {
    case UI_BUTTON: case UI_CHECK: case UI_RADIO: case UI_TEXTAREA:
    case UI_SLIDER: case UI_SPINNER: case UI_DROPDOWN: case UI_TABS: case UI_LIST:
    case UI_CANVAS:
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

static void clamp_win(unoui_ui *ui, unoui_window *w)
{
    if (w->r.x < -w->r.w + 48) w->r.x = -w->r.w + 48;
    if (w->r.x > ui->screen_w - 48) w->r.x = ui->screen_w - 48;
    if (w->r.y < 0) w->r.y = 0;
    if (w->r.y > ui->screen_h - 16) w->r.y = ui->screen_h - 16;
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
    return ui_edit_inner(unoui_widget_rect(ui->theme, win, w), ui->theme);
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
static unoui_action set_list(unoui_ui *ui, int y)
{
    unoui_window *win = ui->win[ui->cap_win]; unoui_widget *w = &win->w[ui->cap_wi];
    unoui_rect r = unoui_widget_rect(ui->theme, win, w);
    int idx = (y - (r.y + 3)) / 11;
    if (idx < 0) idx = 0;
    if (idx > w->nitems - 1) idx = w->nitems - 1;
    w->sel = idx; { unoui_action a = change(w); a.value = idx; return a; }
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

    case UI_FIELD: case UI_TEXTAREA: {
        unoui_rect in = ui_edit_inner(r, t);
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

    case UI_LIST:
        ui->cap_mode = UI_CAP_LIST; return set_list(ui, ev->y);

    case UI_TABS: {
        int x = r.x, k;
        ui->cap_mode = UI_CAP_NONE;
        for (k = 0; k < w->nitems; k++) {
            int tw = fb_text_w(w->items[k]) + 16;
            if (ev->x >= x && ev->x < x + tw) {
                w->sel = k; { unoui_action a = change(w); a.value = k; return a; }
            }
            x += tw;
        }
        return NO_ACT;
    }
    case UI_DROPDOWN:
        ui->cap_mode = UI_CAP_NONE;
        ui->popup_win = ui->focus_win; ui->popup_wi = hi; ui->popup_menu = -1;
        ui->popup_items = w->items; ui->popup_n = w->nitems; ui->popup_hot = w->sel;
        { unoui_rect pr = { r.x, r.y + r.h, r.w, w->nitems * 12 + 4 }; ui->popup_r = pr; }
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
            { unoui_rect pr = { tx, r.y + r.h, mw, m->nitems * 12 + 4 }; ui->popup_r = pr; }
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
    case UI_TABS:
        if (ev->key == UI_KEY_LEFT && w->sel > 0)            { w->sel--; { unoui_action a = change(w); a.value = w->sel; return a; } }
        if (ev->key == UI_KEY_RIGHT && w->sel < w->nitems-1) { w->sel++; { unoui_action a = change(w); a.value = w->sel; return a; } }
        break;
    case UI_LIST: case UI_DROPDOWN:
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
            if (w->kind == UI_VSCROLL || w->kind == UI_HSCROLL) {
                w->value += ev->wheel * (w->vmax / 10 + 1);
                if (w->value < 0) w->value = 0;
                if (w->value > w->vmax) w->value = w->vmax;
                return change(w);
            }
        }
        return NO_ACT;
    }

    case UI_EV_MOUSE_MOVE: {
        ui->mx = ev->x; ui->my = ev->y;
        switch (ui->cap_mode) {
        case UI_CAP_WINDOW: {
            unoui_window *win = ui->win[ui->cap_win];
            /* move only the outline; the window commits on release */
            ui->drag_x = ev->x - ui->grab_dx; ui->drag_y = ev->y - ui->grab_dy;
            if (ui->drag_x < -ui->drag_w + 48) ui->drag_x = -ui->drag_w + 48;
            if (ui->drag_x > ui->screen_w - 48) ui->drag_x = ui->screen_w - 48;
            if (ui->drag_y < 0) ui->drag_y = 0;
            if (ui->drag_y > ui->screen_h - 16) ui->drag_y = ui->screen_h - 16;
            (void)win; return NO_ACT;
        }
        case UI_CAP_RESIZE: {
            unoui_window *win = ui->win[ui->cap_win];
            int nw = (ev->x - ui->grab_dx) - win->r.x;
            int nh = (ev->y - ui->grab_dy) - win->r.y;
            if (nw < win->min_w) nw = win->min_w;
            if (nh < win->min_h) nh = win->min_h;
            if (win->r.x + nw > ui->screen_w) nw = ui->screen_w - win->r.x;
            if (win->r.y + nh > ui->screen_h) nh = ui->screen_h - win->r.y;
            win->r.w = nw; win->r.h = nh;
            unoui_reflow_window(ui->theme, win);
            return NO_ACT;
        }
        case UI_CAP_VTHUMB: return set_vscroll(ui, ev->y);
        case UI_CAP_HTHUMB: return set_hscroll(ui, ev->x);
        case UI_CAP_SLIDER: return set_slider(ui, ev->x);
        case UI_CAP_LIST:   return set_list(ui, ev->y);
        case UI_CAP_TEXT: {
            unoui_window *win = ui->win[ui->cap_win];
            unoui_widget *w = &win->w[ui->cap_wi];
            unoui_rect in = ui_edit_inner(unoui_widget_rect(ui->theme, win, w), ui->theme);
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
            int idx = (ev->y - (ui->popup_r.y + 2)) / 12;
            ui->popup_hot = (idx >= 0 && idx < ui->popup_n) ? idx : -1;
        }
        return NO_ACT;
    }

    case UI_EV_MOUSE_DOWN: {
        unoui_window *win; int wn, hi;
        ui->mx = ev->x; ui->my = ev->y; ui->mdown = 1;

        if (ui->popup_wi >= 0) {                 /* a popup is open */
            if (pt_in(ui->popup_r, ev->x, ev->y)) {
                int idx = (ev->y - (ui->popup_r.y + 2)) / 12;
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
            int gs = 16;                             /* bottom-right resize grip */
            if (ev->x >= win->r.x + win->r.w - gs && ev->x < win->r.x + win->r.w &&
                ev->y >= win->r.y + win->r.h - gs && ev->y < win->r.y + win->r.h) {
                ui->cap_mode = UI_CAP_RESIZE; ui->cap_win = ui->focus_win;
                ui->grab_dx = ev->x - (win->r.x + win->r.w);
                ui->grab_dy = ev->y - (win->r.y + win->r.h);
                return NO_ACT;
            }
        }

        if (!(win->flags & UI_WIN_BARE) &&           /* bare windows don't drag */
            ev->y >= win->r.y && ev->y < win->r.y + ui->theme->m.title_h) {
            const unoui_theme *t = ui->theme;
            int cs = t->m.closebox, fw = t->m.frame_w, th = t->m.title_h;
            if (cs) {                                /* title-bar close box */
                int cbx = win->r.x + fw + 4, cby = win->r.y + fw + (th - fw - cs) / 2;
                if (ev->x >= cbx && ev->x < cbx + cs && ev->y >= cby && ev->y < cby + cs) {
                    unoui_action a; a.changed = 1; a.id = 0;
                    a.kind = UI_ACT_CLOSE; a.value = ui->focus_win; return a;
                }
            }
            ui->cap_mode = UI_CAP_WINDOW; ui->cap_win = ui->focus_win;
            ui->grab_dx = ev->x - win->r.x; ui->grab_dy = ev->y - win->r.y;
            ui->drag_active = 1;                     /* rubber-band outline drag */
            ui->drag_x = win->r.x; ui->drag_y = win->r.y;
            ui->drag_w = win->r.w; ui->drag_h = win->r.h;
            return NO_ACT;
        }
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
        else if (ui->cap_mode == UI_CAP_BUTTON &&
            hit_widget(ui, ui->win[ui->cap_win], ev->x, ev->y) == ui->cap_wi)
            a = activate(ui, ui->cap_win, ui->cap_wi);
        else if (ui->cap_mode == UI_CAP_NONE) canvas_forward(ui, ev);  /* canvas release */
        ui->cap_mode = UI_CAP_NONE;
        return a;
    }

    default:
        return NO_ACT;
    }
}

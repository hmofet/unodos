/* ===========================================================================
 * uodlg.c - the Office 97 dialog engine (OFFICE97-PLAN §5 phase 6c).
 *
 * See uodlg.h for the shape of it.  The whole point is that Office's ~30
 * shared dialogs are the same dozen controls in different arrangements, so
 * they arrive as data tables and this file is the only code.
 * ======================================================================== */
#include "uodlg.h"

static int dstrlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void dstrcpy(char *d, const char *s, int cap)
{
    int i = 0;
    if (!d || cap <= 0) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* ---- metrics --------------------------------------------------------------- */
static int d_title_h(const uoc_look *k) { return fb_text_h() + k->pad + 2; }
static int d_tab_h  (const uoc_look *k) { return fb_text_h() + k->pad + 2; }
static int d_box    (void)              { return fb_text_h() + 3; }  /* check/radio */
static int d_row    (const uoc_look *k) { return fb_text_h() + k->pad; }
#define D_BORDER 3

/* ---- small shapes ---------------------------------------------------------- */
static void fill_circle(int cx, int cy, int r, fb_px c)
{
    int x, y;
    for (y = -r; y <= r; y++)
        for (x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) fb_pixel(cx + x, cy + y, c);
}
static void ring(int cx, int cy, int r, fb_px c)
{
    int x, y;
    for (y = -r; y <= r; y++)
        for (x = -r; x <= r; x++) {
            int d = x * x + y * y;
            if (d <= r * r && d > (r - 1) * (r - 1)) fb_pixel(cx + x, cy + y, c);
        }
}
static void tick(const uoc_look *k, int x, int y, int s, fb_px c)
{
    int i, n = s / 4;
    (void)k;
    if (n < 2) n = 2;
    for (i = 0; i < n; i++) {
        fb_pixel(x + s / 4 + i, y + s / 2 + i, c);
        fb_pixel(x + s / 4 + i, y + s / 2 + i + 1, c);
    }
    for (i = 0; i < n * 2 && s / 4 + n + i < s - 1; i++) {
        fb_pixel(x + s / 4 + n + i, y + s / 2 + n - i, c);
        fb_pixel(x + s / 4 + n + i, y + s / 2 + n - i + 1, c);
    }
}
static void spin_arrow(int x, int y, int up, fb_px c)
{
    int i, n = 3;
    for (i = 0; i < n; i++)
        fb_hline(x + n - 1 - i, y + (up ? i : n - 1 - i), i * 2 + 1, c);
}
/* Trim to fit, for the same reason uochrome does: fb_text clips to the
 * screen, not to any control's rect. */
static const char *fit(const char *s, int avail, char *out, int cap)
{
    int n = 0;
    if (!s) return "";
    while (s[n] && n < cap - 1) out[n] = s[n], n++;
    out[n] = 0;
    while (n > 0 && fb_text_w(out) > avail) out[--n] = 0;
    return out;
}

/* ---- geometry: the one source both the painter and the hit-test use ------- */
static int item_visible(const uod_ui *s, int i)
{
    const uod_item *it = &s->d->item[i];
    if (!s->d->tab || s->d->ntab <= 0) return 1;
    return it->page < 0 || it->page == s->page;
}
static void uod_item_rect(const uod_ui *s, int i, int *x, int *y, int *w, int *h)
{
    const uod_item *it = &s->d->item[i];
    int oy = s->y + D_BORDER + d_title_h(s->look);
    if (s->d->tab && s->d->ntab > 0) oy += d_tab_h(s->look);
    *x = s->x + D_BORDER + it->x;
    *y = oy + it->y;
    *w = it->w;
    *h = it->h > 0 ? it->h : d_row(s->look);
}
static int tab_rect(const uod_ui *s, int t, int *x, int *y, int *w, int *h)
{
    const uoc_look *k = s->look;
    int i, tx = s->x + D_BORDER + 2;
    if (!s->d->tab || t < 0 || t >= s->d->ntab) return 0;
    for (i = 0; i < t; i++) tx += uoc_label_w(s->d->tab[i]) + k->pad * 3;
    *x = tx;
    *y = s->y + D_BORDER + d_title_h(k);
    *w = uoc_label_w(s->d->tab[t]) + k->pad * 3;
    *h = d_tab_h(k);
    return 1;
}
static int hit_item(const uod_ui *s, int mx, int my)
{
    int i, x, y, w, h;
    for (i = 0; i < s->d->n && i < UOD_MAXITEM; i++) {
        const uod_item *it = &s->d->item[i];
        if (!item_visible(s, i)) continue;
        if (it->kind == UOD_LABEL || it->kind == UOD_GROUP) continue;
        if (it->flags & UOD_DISABLED) continue;
        uod_item_rect(s, i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h) return i;
    }
    return -1;
}

/* the drop-down a dialog combo opens */
static void dcombo_rect(const uod_ui *s, int i, int *x, int *y, int *w, int *h)
{
    const uod_item *it = &s->d->item[i];
    int ix, iy, iw, ih, rows = it->nlist > 8 ? 8 : it->nlist;
    uod_item_rect(s, i, &ix, &iy, &iw, &ih);
    *x = ix; *y = iy + ih; *w = iw;
    *h = rows * (fb_text_h() + 2) + 2;
}

/* ---- open / close ---------------------------------------------------------- */
static int find_item(const uod_ui *s, int id)
{
    int i;
    for (i = 0; i < s->d->n && i < UOD_MAXITEM; i++)
        if (s->d->item[i].id == id) return i;
    return -1;
}

void uod_open(uod_ui *s, const uod_dlg *d, int sw, int sh)
{
    int i, ne = 0;
    if (!s || !d) return;
    for (i = 0; i < (int)sizeof *s; i++) ((char *)s)[i] = 0;
    s->d = d;
    s->look = uoc_look_97();
    s->x = (sw - d->w) / 2;
    s->y = (sh - d->h) / 3;          /* a third down, as Windows centred them */
    if (s->x < 0) s->x = 0;
    if (s->y < 0) s->y = 0;
    s->focus = s->hot = s->down = -1;
    s->pop = s->pop_hot = -1;
    s->drag = 0;
    s->open = 1;
    s->result = 0;
    for (i = 0; i < UOD_MAXITEM; i++) s->edit_of[i] = -1;
    for (i = 0; i < d->n && i < UOD_MAXITEM; i++) {
        const uod_item *it = &d->item[i];
        if (it->kind == UOD_EDIT || it->kind == UOD_SPIN) {
            if (ne < UOD_MAXEDIT) {
                s->edit_of[i] = ne;
                dstrcpy(s->text[ne], it->text, UOD_EDITLEN);
                ne++;
            }
        }
        if (it->kind == UOD_SPIN) s->val[i] = it->lo;
        if (s->focus < 0 && it->kind != UOD_LABEL && it->kind != UOD_GROUP &&
            !(it->flags & UOD_DISABLED) && item_visible(s, i))
            s->focus = i;
    }
}
void uod_close(uod_ui *s) { if (s) { s->open = 0; s->pop = -1; } }
int  uod_is_open(const uod_ui *s) { return s && s->open; }
int  uod_result(const uod_ui *s) { return s ? s->result : 0; }

int uod_value(const uod_ui *s, int id)
{
    int i = (s && s->d) ? find_item(s, id) : -1;
    return i >= 0 ? s->val[i] : 0;
}
void uod_set_value(uod_ui *s, int id, int v)
{
    int i = (s && s->d) ? find_item(s, id) : -1;
    if (i >= 0) s->val[i] = v;
}
const char *uod_text(const uod_ui *s, int id)
{
    int i = (s && s->d) ? find_item(s, id) : -1;
    if (i < 0 || s->edit_of[i] < 0) return "";
    return s->text[s->edit_of[i]];
}
void uod_set_text(uod_ui *s, int id, const char *t)
{
    int i = (s && s->d) ? find_item(s, id) : -1;
    if (i >= 0 && s->edit_of[i] >= 0) dstrcpy(s->text[s->edit_of[i]], t, UOD_EDITLEN);
}
int uod_preview_rect(const uod_ui *s, int id, int *x, int *y, int *w, int *h)
{
    int i = (s && s->d) ? find_item(s, id) : -1;
    int rx, ry, rw, rh;
    if (i < 0) return 0;
    uod_item_rect(s, i, &rx, &ry, &rw, &rh);
    /* one per line: -Wall flags `if (a) x; if (b) y;` as misleadingly
     * indented, and it is right to (unoui's build notes record the same) */
    if (x) *x = rx + 2;
    if (y) *y = ry + 2;
    if (w) *w = rw - 4;
    if (h) *h = rh - 4;
    return 1;
}

/* ---- painting -------------------------------------------------------------- */
static void paint_item(const uod_ui *s, int i)
{
    const uoc_look *k = s->look;
    const uod_item *it = &s->d->item[i];
    int x, y, w, h, dis = (it->flags & UOD_DISABLED) != 0;
    fb_px fg = dis ? k->gray_text : k->text;
    char cut[96];
    uod_item_rect(s, i, &x, &y, &w, &h);

    switch (it->kind) {
    case UOD_LABEL:
        if (dis) uoc_draw_label(x + 1, y + 1, it->text, k->hilight, 1);
        uoc_draw_label(x, y, it->text, fg, 1);
        break;

    case UOD_GROUP: {
        /* the etched frame, then the caption punched back out of its top
         * edge - which is how a Windows group box gets its notch */
        int ly = y + fb_text_h() / 2, lw = uoc_label_w(it->text);
        uoc_bevel(x, ly, w, h - (ly - y), k->shadow, k->hilight, 1);
        fb_fill_rect(x + k->pad, y, lw + k->pad, fb_text_h(), k->face);
        uoc_draw_label(x + k->pad + 2, y, it->text, fg, 1);
        break;
    }

    case UOD_BUTTON: {
        int down = (s->down == i);
        if (it->flags & UOD_DEFAULT)
            fb_frame_rect(x - 1, y - 1, w + 2, h + 2, k->dkshadow);
        fb_fill_rect(x, y, w, h, k->face);
        if (down) uoc_sunken(k, x, y, w, h);
        else      uoc_raised(k, x, y, w, h);
        {   int lw = uoc_label_w(it->text);
            int tx = x + (w - lw) / 2 + (down ? 1 : 0);
            int ty = y + (h - fb_text_h()) / 2 + (down ? 1 : 0);
            if (dis) uoc_draw_label(tx + 1, ty + 1, it->text, k->hilight, 1);
            uoc_draw_label(tx, ty, it->text, fg, 1);
        }
        if (s->focus == i)
            fb_frame_rect(x + 3, y + 3, w - 6, h - 6, k->dkshadow);
        break;
    }

    case UOD_CHECK: {
        int b = d_box();
        fb_fill_rect(x, y, b, b, dis ? k->face : k->hilight);
        uoc_sunken(k, x, y, b, b);
        if (s->val[i]) tick(k, x + 1, y + 1, b - 2, fg);
        uoc_draw_label(x + b + k->pad, y + (b - fb_text_h()) / 2, it->text, fg, 1);
        if (s->focus == i)
            fb_frame_rect(x + b + k->pad - 2, y, uoc_label_w(it->text) + 4, b,
                          k->dkshadow);
        break;
    }

    case UOD_RADIO: {
        int b = d_box(), cx = x + b / 2, cy = y + b / 2;
        fill_circle(cx, cy, b / 2, dis ? k->face : k->hilight);
        ring(cx, cy, b / 2, k->shadow);
        ring(cx, cy, b / 2 - 1, k->dkshadow);
        if (s->val[i]) fill_circle(cx, cy, b / 5, fg);
        uoc_draw_label(x + b + k->pad, y + (b - fb_text_h()) / 2, it->text, fg, 1);
        if (s->focus == i)
            fb_frame_rect(x + b + k->pad - 2, y, uoc_label_w(it->text) + 4, b,
                          k->dkshadow);
        break;
    }

    case UOD_EDIT: {
        const char *t = (s->edit_of[i] >= 0) ? s->text[s->edit_of[i]] : "";
        fb_fill_rect(x, y, w, h, dis ? k->face : k->hilight);
        uoc_sunken(k, x, y, w, h);
        fb_text(x + 3, y + (h - fb_text_h()) / 2,
                fit(t, w - 8, cut, (int)sizeof cut), fg, -1);
        if (s->focus == i && !dis) {
            int cw = fb_text_w(fit(t, w - 8, cut, (int)sizeof cut));
            fb_vline(x + 3 + cw, y + 3, h - 6, k->text);
        }
        break;
    }

    case UOD_SPIN: {
        const char *t = (s->edit_of[i] >= 0) ? s->text[s->edit_of[i]] : "";
        int bw = fb_text_h(), fw = w - bw;
        fb_fill_rect(x, y, fw, h, dis ? k->face : k->hilight);
        uoc_sunken(k, x, y, fw, h);
        fb_text(x + 3, y + (h - fb_text_h()) / 2,
                fit(t, fw - 8, cut, (int)sizeof cut), fg, -1);
        fb_fill_rect(x + fw, y, bw, h, k->face);
        uoc_raised(k, x + fw, y, bw, h / 2);
        uoc_raised(k, x + fw, y + h / 2, bw, h - h / 2);
        spin_arrow(x + fw + bw / 2 - 2, y + h / 4 - 1, 1, fg);
        spin_arrow(x + fw + bw / 2 - 2, y + h - h / 4 - 2, 0, fg);
        break;
    }

    case UOD_LIST: {
        int lh = fb_text_h() + 2, r, rows = (h - 4) / lh;
        fb_fill_rect(x, y, w, h, dis ? k->face : k->hilight);
        uoc_sunken(k, x, y, w, h);
        for (r = 0; r < rows && r < it->nlist; r++) {
            int ry = y + 2 + r * lh;
            fb_px c = fg;
            if (r == s->val[i]) {
                fb_fill_rect(x + 2, ry, w - 4, lh, k->sel);
                c = k->sel_text;
            }
            fb_text(x + 4, ry + 1, fit(it->list[r], w - 8, cut, (int)sizeof cut),
                    c, -1);
        }
        break;
    }

    case UOD_COMBO: {
        int bw = fb_text_h() + 2, fw = w - bw;
        const char *t = (it->list && s->val[i] < it->nlist)
                      ? it->list[s->val[i]] : it->text;
        fb_fill_rect(x, y, fw, h, dis ? k->face : k->hilight);
        uoc_sunken(k, x, y, fw, h);
        fb_text(x + 3, y + (h - fb_text_h()) / 2,
                fit(t, fw - 8, cut, (int)sizeof cut), fg, -1);
        fb_fill_rect(x + fw, y, bw, h, k->face);
        if (s->pop == i) uoc_sunken(k, x + fw, y, bw, h);
        else             uoc_raised(k, x + fw, y, bw, h);
        {   int i2, n = 4;
            for (i2 = 0; i2 < n; i2++)
                fb_hline(x + fw + bw / 2 - n / 2 + i2, y + h / 2 - 2 + i2,
                         (n - i2) * 2 - 1, fg);
        }
        break;
    }

    case UOD_PREVIEW:
        fb_fill_rect(x, y, w, h, k->hilight);
        uoc_sunken(k, x, y, w, h);
        break;

    default: break;
    }
}

void uod_render(const uod_ui *s)
{
    const uoc_look *k;
    int i, x, y, w, h, th;
    if (!s || !s->open || !s->d) return;
    k = s->look;
    th = d_title_h(k);

    /* the frame */
    fb_fill_rect(s->x, s->y, s->d->w, s->d->h, k->face);
    uoc_raised(k, s->x, s->y, s->d->w, s->d->h);

    /* the title bar, with the "?" and close buttons Office dialogs carried */
    fb_fill_rect(s->x + D_BORDER, s->y + D_BORDER,
                 s->d->w - 2 * D_BORDER, th - 2, k->sel);
    fb_text(s->x + D_BORDER + 4, s->y + D_BORDER + 1, s->d->title, k->sel_text, -1);
    {
        int bs = fb_text_h(), bx = s->x + s->d->w - D_BORDER - bs - 2;
        int by = s->y + D_BORDER;
        fb_fill_rect(bx, by, bs, bs, k->face);
        uoc_raised(k, bx, by, bs, bs);
        fb_text(bx + 2, by + 1, "x", k->text, -1);
        if (s->d->help) {
            bx -= bs + 2;
            fb_fill_rect(bx, by, bs, bs, k->face);
            uoc_raised(k, bx, by, bs, bs);
            fb_text(bx + 2, by + 1, "?", k->text, -1);
        }
    }

    /* tabs: the active one is drawn one pixel taller and open at the bottom,
     * which is what makes it read as part of the page rather than above it */
    if (s->d->tab && s->d->ntab > 0) {
        int ty = s->y + D_BORDER + th, page_y = ty + d_tab_h(k) - 2;
        fb_fill_rect(s->x + D_BORDER, page_y,
                     s->d->w - 2 * D_BORDER,
                     s->d->h - (page_y - s->y) - D_BORDER, k->face);
        uoc_bevel(s->x + D_BORDER, page_y, s->d->w - 2 * D_BORDER,
                  s->d->h - (page_y - s->y) - D_BORDER,
                  k->hilight, k->shadow, 1);
        for (i = 0; i < s->d->ntab; i++) {
            int on = (i == s->page);
            if (!tab_rect(s, i, &x, &y, &w, &h)) continue;
            fb_fill_rect(x, y + (on ? 0 : 2), w, h - (on ? 0 : 2), k->face);
            uoc_bevel(x, y + (on ? 0 : 2), w, h - (on ? 0 : 2) + 2,
                      k->hilight, k->shadow, 1);
            if (on) fb_hline(x + 1, page_y, w - 2, k->face);
            uoc_draw_label(x + k->pad + 2, y + (on ? 2 : 4), s->d->tab[i],
                           k->text, 1);
        }
    }

    for (i = 0; i < s->d->n && i < UOD_MAXITEM; i++)
        if (item_visible(s, i)) paint_item(s, i);

    /* an open combo list paints over everything else in the dialog */
    if (s->pop >= 0) {
        const uod_item *it = &s->d->item[s->pop];
        int lh = fb_text_h() + 2, r, rows = it->nlist > 8 ? 8 : it->nlist;
        dcombo_rect(s, s->pop, &x, &y, &w, &h);
        fb_fill_rect(x, y, w, h, k->hilight);
        fb_frame_rect(x, y, w, h, k->dkshadow);
        for (r = 0; r < rows; r++) {
            int ry = y + 1 + r * lh;
            fb_px c = k->text;
            if (r == s->pop_hot) {
                fb_fill_rect(x + 1, ry, w - 2, lh, k->sel);
                c = k->sel_text;
            }
            fb_text(x + 3, ry + 1, it->list[r], c, -1);
        }
    }
}

/* ---- events ---------------------------------------------------------------- */
static int focusable(const uod_ui *s, int i)
{
    const uod_item *it = &s->d->item[i];
    return item_visible(s, i) && !(it->flags & UOD_DISABLED) &&
           it->kind != UOD_LABEL && it->kind != UOD_GROUP &&
           it->kind != UOD_PREVIEW;
}
static void move_focus(uod_ui *s, int d)
{
    int i, n = s->d->n < UOD_MAXITEM ? s->d->n : UOD_MAXITEM, cur = s->focus;
    for (i = 0; i < n; i++) {
        cur += d;
        if (cur < 0) cur = n - 1;
        if (cur >= n) cur = 0;
        if (focusable(s, cur)) { s->focus = cur; return; }
    }
}
/* A radio takes its whole group with it: Windows clears the siblings. */
static void set_radio(uod_ui *s, int i)
{
    int j, g = s->d->item[i].group;
    for (j = 0; j < s->d->n && j < UOD_MAXITEM; j++)
        if (s->d->item[j].kind == UOD_RADIO && s->d->item[j].group == g)
            s->val[j] = (j == i);
}
static int activate(uod_ui *s, int i)
{
    const uod_item *it = &s->d->item[i];
    switch (it->kind) {
    case UOD_BUTTON: s->result = it->id; uod_close(s); return 1;
    case UOD_CHECK:  s->val[i] = !s->val[i]; return 1;
    case UOD_RADIO:  set_radio(s, i); return 1;
    default: break;
    }
    return 0;
}
static int default_button(const uod_ui *s)
{
    int i;
    for (i = 0; i < s->d->n && i < UOD_MAXITEM; i++)
        if (s->d->item[i].kind == UOD_BUTTON &&
            (s->d->item[i].flags & UOD_DEFAULT)) return i;
    return -1;
}

int uod_handle(uod_ui *s, const unoui_event *e)
{
    const uoc_look *k;
    int i, x, y, w, h;
    if (!s || !s->open || !s->d || !e) return 0;
    k = s->look;

    if (e->kind == UI_EV_MOUSE_MOVE) {
        if (s->drag) {
            s->x = e->x - s->drag_dx;
            s->y = e->y - s->drag_dy;
            return 1;
        }
        if (s->pop >= 0) {
            dcombo_rect(s, s->pop, &x, &y, &w, &h);
            s->pop_hot = (e->x >= x && e->x < x + w && e->y >= y && e->y < y + h)
                       ? (e->y - y - 1) / (fb_text_h() + 2) : -1;
            if (s->pop_hot >= s->d->item[s->pop].nlist) s->pop_hot = -1;
            return 1;
        }
        s->hot = hit_item(s, e->x, e->y);
        return 1;
    }

    if (e->kind == UI_EV_MOUSE_UP) {
        if (s->drag) { s->drag = 0; return 1; }
        if (s->down >= 0) {
            int was = (hit_item(s, e->x, e->y) == s->down);
            int d = s->down;
            s->down = -1;
            if (was) activate(s, d);
            return 1;
        }
        return 1;
    }

    if (e->kind != UI_EV_MOUSE_DOWN) {
        /* keyboard */
        if (e->kind == UI_EV_KEY) {
            switch (e->key) {
            case UI_KEY_ESC:
                if (s->pop >= 0) { s->pop = -1; return 1; }
                s->result = UOD_ID_CANCEL; uod_close(s); return 1;
            case UI_KEY_ENTER:
                if (s->pop >= 0) {
                    if (s->pop_hot >= 0) s->val[s->pop] = s->pop_hot;
                    s->pop = -1; return 1;
                }
                if (s->focus >= 0 && s->d->item[s->focus].kind == UOD_BUTTON)
                    { activate(s, s->focus); return 1; }
                i = default_button(s);
                if (i >= 0) activate(s, i);
                return 1;
            case UI_KEY_TAB:
                move_focus(s, (e->mods & UI_MOD_SHIFT) ? -1 : 1);
                return 1;
            case UI_KEY_UP:
            case UI_KEY_DOWN: {
                int d = (e->key == UI_KEY_DOWN) ? 1 : -1;
                if (s->pop >= 0) {
                    s->pop_hot += d;
                    if (s->pop_hot < 0) s->pop_hot = 0;
                    if (s->pop_hot >= s->d->item[s->pop].nlist)
                        s->pop_hot = s->d->item[s->pop].nlist - 1;
                    return 1;
                }
                if (s->focus < 0) return 1;
                switch (s->d->item[s->focus].kind) {
                case UOD_LIST:
                case UOD_COMBO: {
                    int v = s->val[s->focus] + d, n = s->d->item[s->focus].nlist;
                    if (v >= 0 && v < n) s->val[s->focus] = v;
                    return 1;
                }
                case UOD_SPIN: {
                    const uod_item *it = &s->d->item[s->focus];
                    int v = s->val[s->focus] + d;
                    if (v < it->lo) v = it->lo;
                    if (v > it->hi) v = it->hi;
                    s->val[s->focus] = v;
                    if (s->edit_of[s->focus] >= 0) {
                        char b[16]; int n = 0, q = v, dg[8], j = 0;
                        if (q < 0) { b[n++] = '-'; q = -q; }
                        do { dg[j++] = q % 10; q /= 10; } while (q && j < 8);
                        while (j) b[n++] = (char)('0' + dg[--j]);
                        b[n] = 0;
                        dstrcpy(s->text[s->edit_of[s->focus]], b, UOD_EDITLEN);
                    }
                    return 1;
                }
                case UOD_RADIO: {
                    int j, g = s->d->item[s->focus].group;
                    for (j = s->focus + d; j >= 0 && j < s->d->n; j += d)
                        if (s->d->item[j].kind == UOD_RADIO &&
                            s->d->item[j].group == g && focusable(s, j)) {
                            s->focus = j; set_radio(s, j); return 1;
                        }
                    return 1;
                }
                default: move_focus(s, d); return 1;
                }
            }
            default: return 1;
            }
        }
        if (e->kind == UI_EV_CHAR) {
            int c = e->ch;
            if (s->focus >= 0 && s->edit_of[s->focus] >= 0 && c >= ' ') {
                char *t = s->text[s->edit_of[s->focus]];
                int n = dstrlen(t);
                if (n < UOD_EDITLEN - 1) { t[n] = (char)c; t[n + 1] = 0; }
                return 1;
            }
            if (c == 8 && s->focus >= 0 && s->edit_of[s->focus] >= 0) {
                char *t = s->text[s->edit_of[s->focus]];
                int n = dstrlen(t);
                if (n) t[n - 1] = 0;
                return 1;
            }
            /* a mnemonic anywhere in the dialog jumps to and fires it */
            if (c >= ' ') {
                int up = (c >= 'a' && c <= 'z') ? c - 32 : c;
                for (i = 0; i < s->d->n && i < UOD_MAXITEM; i++)
                    if (focusable(s, i) &&
                        uoc_mnemonic_of(s->d->item[i].text) == up) {
                        s->focus = i;
                        activate(s, i);
                        return 1;
                    }
            }
            return 1;
        }
        return 1;                       /* modal: swallow everything else    */
    }

    /* ---- mouse down ------------------------------------------------------ */
    if (s->pop >= 0) {
        dcombo_rect(s, s->pop, &x, &y, &w, &h);
        if (e->x >= x && e->x < x + w && e->y >= y && e->y < y + h) {
            int row = (e->y - y - 1) / (fb_text_h() + 2);
            if (row >= 0 && row < s->d->item[s->pop].nlist) s->val[s->pop] = row;
        }
        s->pop = -1;
        return 1;
    }
    /* the title bar drags; the close and help buttons in it do their thing */
    {
        int th = d_title_h(k), bs = fb_text_h();
        int bx = s->x + s->d->w - D_BORDER - bs - 2, by = s->y + D_BORDER;
        if (e->y >= by && e->y < by + bs) {
            if (e->x >= bx && e->x < bx + bs) {
                s->result = UOD_ID_CANCEL; uod_close(s); return 1;
            }
            if (s->d->help && e->x >= bx - bs - 2 && e->x < bx - 2) {
                s->result = UOD_ID_HELP; uod_close(s); return 1;
            }
        }
        if (e->x >= s->x && e->x < s->x + s->d->w &&
            e->y >= s->y + D_BORDER && e->y < s->y + D_BORDER + th) {
            s->drag = 1;
            s->drag_dx = e->x - s->x; s->drag_dy = e->y - s->y;
            return 1;
        }
    }
    /* a tab */
    if (s->d->tab) {
        for (i = 0; i < s->d->ntab; i++) {
            if (!tab_rect(s, i, &x, &y, &w, &h)) continue;
            if (e->x >= x && e->x < x + w && e->y >= y && e->y < y + h) {
                s->page = i;
                s->focus = -1;
                for (i = 0; i < s->d->n && i < UOD_MAXITEM; i++)
                    if (focusable(s, i)) { s->focus = i; break; }
                return 1;
            }
        }
    }
    i = hit_item(s, e->x, e->y);
    if (i >= 0) {
        const uod_item *it = &s->d->item[i];
        s->focus = i;
        uod_item_rect(s, i, &x, &y, &w, &h);
        switch (it->kind) {
        case UOD_BUTTON: s->down = i; break;
        case UOD_CHECK:  s->val[i] = !s->val[i]; break;
        case UOD_RADIO:  set_radio(s, i); break;
        case UOD_LIST: {
            int row = (e->y - y - 2) / (fb_text_h() + 2);
            if (row >= 0 && row < it->nlist) s->val[i] = row;
            break;
        }
        case UOD_COMBO:
            s->pop = i;
            s->pop_hot = s->val[i];
            break;
        case UOD_SPIN: {
            int bw = fb_text_h(), fw = w - bw, v = s->val[i];
            if (e->x >= x + fw) {
                v += (e->y < y + h / 2) ? 1 : -1;
                if (v < it->lo) v = it->lo;
                if (v > it->hi) v = it->hi;
                s->val[i] = v;
                if (s->edit_of[i] >= 0) {
                    char b[16]; int n = 0, q = v, dg[8], j = 0;
                    if (q < 0) { b[n++] = '-'; q = -q; }
                    do { dg[j++] = q % 10; q /= 10; } while (q && j < 8);
                    while (j) b[n++] = (char)('0' + dg[--j]);
                    b[n] = 0;
                    dstrcpy(s->text[s->edit_of[i]], b, UOD_EDITLEN);
                }
            }
            break;
        }
        default: break;
        }
    }
    return 1;
}

/* ---- message boxes ---------------------------------------------------------
 * Built into a static dialog rather than declared by the app, because "Save
 * changes to Document1?" is not anybody's data table. */
static uod_item  g_mb_item[5];
static uod_dlg   g_mb;
static char      g_mb_title[64];
static char      g_mb_text[192];

void uod_msgbox(uod_ui *s, const char *title, const char *text,
                int buttons, int sw, int sh)
{
    const uoc_look *k = uoc_look_97();
    int n = 0, bw = k->icon_px * 4, bh = fb_text_h() + k->pad * 2;
    int tw = fb_text_w(text) + k->pad * 6, dw, i;
    int nb = (buttons == UOD_MB_OK) ? 1
           : (buttons == UOD_MB_YESNOCANCEL) ? 3 : 2;
    if (tw < bw * nb + k->pad * 4) tw = bw * nb + k->pad * 4;
    dw = tw;

    dstrcpy(g_mb_title, title, (int)sizeof g_mb_title);
    dstrcpy(g_mb_text,  text,  (int)sizeof g_mb_text);

    g_mb_item[n].kind = UOD_LABEL;
    g_mb_item[n].id = 0;
    g_mb_item[n].text = g_mb_text;
    g_mb_item[n].x = k->pad * 2; g_mb_item[n].y = k->pad * 2;
    g_mb_item[n].w = tw; g_mb_item[n].h = fb_text_h();
    g_mb_item[n].page = -1; g_mb_item[n].flags = 0;
    g_mb_item[n].list = 0; g_mb_item[n].nlist = 0;
    g_mb_item[n].group = 0; g_mb_item[n].lo = 0; g_mb_item[n].hi = 0;
    n++;

    {
        static const char *const okc[3]  = { "OK", "Cancel", 0 };
        static const char *const ync[3]  = { "&Yes", "&No", "Cancel" };
        const char *const *lbl = (buttons == UOD_MB_YESNO ||
                                  buttons == UOD_MB_YESNOCANCEL) ? ync : okc;
        static const int idok[3] = { UOD_ID_OK, UOD_ID_CANCEL, 0 };
        static const int idyn[3] = { UOD_ID_YES, UOD_ID_NO, UOD_ID_CANCEL };
        const int *ids = (buttons == UOD_MB_YESNO ||
                          buttons == UOD_MB_YESNOCANCEL) ? idyn : idok;
        int total = nb * bw + (nb - 1) * k->pad;
        int bx = (dw - total) / 2, by = k->pad * 4 + fb_text_h();
        for (i = 0; i < nb; i++) {
            g_mb_item[n].kind = UOD_BUTTON;
            g_mb_item[n].id = ids[i];
            g_mb_item[n].text = lbl[i];
            g_mb_item[n].x = bx + i * (bw + k->pad);
            g_mb_item[n].y = by;
            g_mb_item[n].w = bw; g_mb_item[n].h = bh;
            g_mb_item[n].page = -1;
            g_mb_item[n].flags = (i == 0) ? UOD_DEFAULT : 0;
            g_mb_item[n].list = 0; g_mb_item[n].nlist = 0;
            g_mb_item[n].group = 0; g_mb_item[n].lo = 0; g_mb_item[n].hi = 0;
            n++;
        }
        g_mb.h = by + bh + k->pad * 2 + d_title_h(k) + D_BORDER * 2;
    }

    g_mb.title = g_mb_title;
    g_mb.item = g_mb_item;
    g_mb.n = n;
    g_mb.tab = 0; g_mb.ntab = 0;
    g_mb.w = dw + D_BORDER * 2;
    g_mb.help = 0;
    uod_open(s, &g_mb, sw, sh);
}

/* ===========================================================================
 * uochrome.c - the Office 97 command-bar engine (OFFICE97-PLAN §5 phase 6).
 *
 * See uochrome.h for what this is and why it is not unoui.  Three things in
 * here are worth knowing before changing any of it:
 *
 *   1. GEOMETRY IS COMPUTED ONCE, by the same functions the painter and the
 *      hit-tester both call.  unoui learned this the expensive way (its
 *      unoui_content_origin comment says so): if a click is tested against
 *      arithmetic that only resembles what was drawn, the two drift and
 *      buttons stop working where they look.  Every toolbar in here is placed
 *      by tb_origin()/tb_btn_rect() and by nothing else, which is what let
 *      docking and floating arrive in 6b without re-deriving a coordinate.
 *   2. METRICS DERIVE FROM THE FONT, never from a pixel count.  On the host
 *      harness fb_text is an 8x8 bitmap; on pc64 it is the kerned TTF engine
 *      at whatever size and UI scale the user picked.
 *   3. EVERY GESTURE IS A PURE FUNCTION OF THE EVENT STREAM.  That is what
 *      makes the host storyboard gate meaningful: it drives the same
 *      uoc_handle() the OS drives.
 * ======================================================================== */
#include "uochrome.h"

/* ---- the look ------------------------------------------------------------- */
static const uoc_look k97 = {
    FB_RGB(0xC0,0xC0,0xC0),   /* face      */
    FB_RGB(0xFF,0xFF,0xFF),   /* hilight   */
    FB_RGB(0xDF,0xDF,0xDF),   /* light     */
    FB_RGB(0x80,0x80,0x80),   /* shadow    */
    FB_RGB(0x00,0x00,0x00),   /* dkshadow  */
    FB_RGB(0x00,0x00,0x00),   /* text      */
    FB_RGB(0x80,0x80,0x80),   /* gray_text */
    FB_RGB(0x00,0x00,0x80),   /* sel       */
    FB_RGB(0xFF,0xFF,0xFF),   /* sel_text  */
    FB_RGB(0xFF,0xFF,0xE1),   /* tip_bg    */
    16,                       /* icon_px   */
    4                         /* pad       */
};
const uoc_look *uoc_look_97(void) { return &k97; }

/* ---- metrics, all derived ------------------------------------------------- */
static int m_bar_h  (const uoc_look *k) { return fb_text_h() + 2 * k->pad + 2; }
static int m_item_h (const uoc_look *k)
{ int t = fb_text_h() + 6, i = k->icon_px + 2; return t > i ? t : i; }
static int m_sep_h  (void)              { return fb_text_h() / 2 + 3; }
static int m_gutter (const uoc_look *k) { return k->icon_px + 6; }
static int m_arrow_w(const uoc_look *k) { return k->pad + 7; }
static int m_accel_gap(const uoc_look *k) { return k->pad * 4; }
static int m_tb_h   (const uoc_look *k) { return k->icon_px + 6; }
static int m_tb_btn (const uoc_look *k) { return k->icon_px + 8; }
static int m_grip_w (const uoc_look *k) { return k->pad + 3; }
static int m_title_h(const uoc_look *k) { return fb_text_h() + k->pad; }
static int m_swatch (const uoc_look *k) { return k->icon_px + 4; }
#define POPUP_BORDER 2
#define TEARBAR_H    7
#define DOCK_MARGIN(k) (m_tb_h(k) + m_tb_h(k) / 2)

/* ---- label parsing ---------------------------------------------------------
 * "&Open...\tCtrl+O" is one string carrying three things: the label, which
 * character is the mnemonic, and the accelerator drawn in its own column. */
static int label_of(const char *s, char *out, int cap, int *mn)
{
    int n = 0;
    *mn = -1;
    while (s && *s && *s != '\t' && n < cap - 1) {
        if (*s == '&') {
            s++;
            if (*s == '&') { out[n++] = *s++; continue; }  /* "&&" is a real & */
            if (*mn < 0) *mn = n;
            continue;
        }
        out[n++] = *s++;
    }
    out[n] = 0;
    return n;
}
static const char *accel_of(const char *s)
{
    while (s && *s && *s != '\t') s++;
    return (s && *s == '\t') ? s + 1 : 0;
}
int uoc_mnemonic_of(const char *s)
{
    char buf[96];
    int mn, c;
    label_of(s, buf, (int)sizeof buf, &mn);
    if (mn < 0) return 0;
    c = (unsigned char)buf[mn];
    if (c >= 'a' && c <= 'z') c -= 32;
    return c;
}
int uoc_draw_label(int x, int y, const char *s, fb_px fg, int show_mn)
{
    char buf[96], pre[96];
    int mn, n, w, i;
    n = label_of(s, buf, (int)sizeof buf, &mn);
    fb_text(x, y, buf, fg, -1);
    w = fb_text_w(buf);
    if (show_mn && mn >= 0 && mn < n) {
        int cx, cw;
        for (i = 0; i < mn; i++) pre[i] = buf[i];
        pre[mn] = 0;
        cx = fb_text_w(pre);
        pre[0] = buf[mn]; pre[1] = 0;
        cw = fb_text_w(pre);
        fb_hline(x + cx, y + fb_text_h(), cw, fg);
    }
    return w;
}
int uoc_label_w(const char *s)
{
    char buf[96]; int mn;
    label_of(s, buf, (int)sizeof buf, &mn);
    return fb_text_w(buf);
}

/* Trim `s` until it fits `avail` pixels.
 *
 * THIS IS NOT AN fb_set_clip JOB, and finding that out cost a rendering bug.
 * fb.c's fb_text clips to the SCREEN only - the settable clip window lives in
 * fb_aa.c and governs the alpha primitives, and fb.c's own comment says the
 * two domains merely "agree in practice" because unoui sizes widgets to fit.
 * A combo field is the case where they do not agree: "Times New Roman" is
 * wider than the Font box, and the overflow paints across the buttons beside
 * it - then shows through the transparent parts of their icons, because those
 * are drawn before it in the same pass. */
static const char *fit_text(const char *s, int avail, char *out, int cap)
{
    int n = 0;
    if (!s) return "";
    while (s[n] && n < cap - 1) out[n] = s[n], n++;
    out[n] = 0;
    while (n > 0 && fb_text_w(out) > avail) out[--n] = 0;
    return out;
}
#define draw_label uoc_draw_label
#define label_w    uoc_label_w

/* ---- bevels ---------------------------------------------------------------- */
void uoc_bevel(int x, int y, int w, int h, fb_px tl, fb_px br, int thick)
{
    int i;
    if (w <= 0 || h <= 0) return;
    for (i = 0; i < thick; i++) {
        fb_hline(x + i, y + i, w - 2 * i, tl);
        fb_vline(x + i, y + i, h - 2 * i, tl);
        fb_hline(x + i, y + h - 1 - i, w - 2 * i, br);
        fb_vline(x + w - 1 - i, y + i, h - 2 * i, br);
    }
}
void uoc_raised(const uoc_look *k, int x, int y, int w, int h)
{
    uoc_bevel(x, y, w, h, k->light, k->dkshadow, 1);
    uoc_bevel(x + 1, y + 1, w - 2, h - 2, k->hilight, k->shadow, 1);
}
void uoc_sunken(const uoc_look *k, int x, int y, int w, int h)
{
    uoc_bevel(x, y, w, h, k->shadow, k->hilight, 1);
    uoc_bevel(x + 1, y + 1, w - 2, h - 2, k->dkshadow, k->light, 1);
}
void uoc_etch_h(const uoc_look *k, int x, int y, int w)
{ fb_hline(x, y, w, k->shadow); fb_hline(x, y + 1, w, k->hilight); }
void uoc_etch_v(const uoc_look *k, int x, int y, int h)
{ fb_vline(x, y, h, k->shadow); fb_vline(x + 1, y, h, k->hilight); }
#define bevel  uoc_bevel
#define raised uoc_raised
#define sunken uoc_sunken
#define etch_h uoc_etch_h
#define etch_v uoc_etch_v

/* the dashed outline Windows drags a toolbar around as */
static void drag_outline(int x, int y, int w, int h, fb_px c)
{
    int i;
    for (i = 0; i < w; i += 4) { fb_hline(x + i, y, 2, c); fb_hline(x + i, y + h - 1, 2, c); }
    for (i = 0; i < h; i += 4) { fb_vline(x, y + i, 2, c); fb_vline(x + w - 1, y + i, 2, c); }
}

/* ---- icons ----------------------------------------------------------------- */
static const fb_px *g_atlas;
static int g_cell, g_cols, g_count;

void uoc_set_icons(const fb_px *atlas, int cell, int cols, int count)
{ g_atlas = atlas; g_cell = cell; g_cols = cols; g_count = count; }

void uoc_draw_icon(const uoc_look *k, int x, int y, int idx, int disabled)
{
    int p = k->icon_px;
    if (idx < 0) return;
    if (g_atlas && g_cell > 0 && g_cols > 0 && idx < g_count) {
        int sx = (idx % g_cols) * g_cell, sy = (idx / g_cols) * g_cell, r, c;
        for (r = 0; r < g_cell && r < p; r++)
            for (c = 0; c < g_cell && c < p; c++) {
                fb_px v = g_atlas[(long)(sy + r) * g_cols * g_cell + sx + c];
                if (v >> 24) fb_pixel(x + c, y + r, disabled ? k->gray_text : v);
            }
        return;
    }
    {   /* no artwork installed: a deterministic placeholder from the index */
        unsigned hsh = (unsigned)idx * 2654435761u;
        int r, c, cellw = (p - 4) / 3;
        fb_px on = disabled ? k->gray_text : k->dkshadow;
        fb_frame_rect(x, y, p, p, disabled ? k->gray_text : k->shadow);
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++, hsh >>= 1)
                if (hsh & 1)
                    fb_fill_rect(x + 2 + c * cellw, y + 2 + r * cellw,
                                 cellw, cellw, on);
    }
}
#define draw_icon(k,x,y,i,d) uoc_draw_icon(k,x,y,i,d)

static void draw_check(const uoc_look *k, int x, int y, fb_px c)
{
    int p = k->icon_px, i, n = p / 3;
    for (i = 0; i < n; i++) {
        fb_pixel(x + p / 3 - n / 2 + i, y + p / 2 + i, c);
        fb_pixel(x + p / 3 - n / 2 + i, y + p / 2 + i + 1, c);
    }
    for (i = 0; i < n * 2; i++) {
        fb_pixel(x + p / 3 - n / 2 + n + i, y + p / 2 + n - i, c);
        fb_pixel(x + p / 3 - n / 2 + n + i, y + p / 2 + n - i + 1, c);
    }
}
static void draw_bullet(const uoc_look *k, int x, int y, fb_px c)
{ fb_fill_rect(x + k->icon_px / 2 - 2, y + k->icon_px / 2 - 2, 4, 4, c); }

static void draw_arrow(int x, int y, int h, fb_px c)
{
    int i, n = 4;
    for (i = 0; i < n; i++)
        fb_vline(x + i, y + h / 2 - (n - 1 - i), (n - i) * 2 - 1, c);
}
static void draw_down(int x, int y, fb_px c)
{
    int i, n = 4;
    for (i = 0; i < n; i++)
        fb_hline(x + i, y + i, (n - i) * 2 - 1, c);
}

/* ---- the item lists at each open level ------------------------------------ */
static const uoc_item *level_items(const uoc_ui *u, int level, int *n)
{
    const uoc_item *it;
    int cnt, k;
    if (u->open < 0 || u->open >= u->nmenu) { *n = 0; return 0; }
    it = u->menu[u->open].item; cnt = u->menu[u->open].n;
    for (k = 0; k < level; k++) {
        int i = u->path[k];
        if (i < 0 || i >= cnt || !it[i].sub) { *n = 0; return 0; }
        cnt = it[i].nsub;
        it  = it[i].sub;
    }
    *n = cnt;
    return it;
}

/* ---- the menu bar --------------------------------------------------------- */
static int bar_item_x(const uoc_ui *u, int idx, int *w)
{
    const uoc_look *k = u->look;
    int i, x = u->x + k->pad;
    for (i = 0; i < idx && i < u->nmenu; i++)
        x += label_w(u->menu[i].title) + k->pad * 3;
    if (w) *w = (idx < u->nmenu) ? label_w(u->menu[idx].title) + k->pad * 3 : 0;
    return x;
}
static int bar_hit(const uoc_ui *u, int mx, int my)
{
    const uoc_look *k = u->look;
    int i, w;
    if (my < u->y || my >= u->y + m_bar_h(k)) return -1;
    for (i = 0; i < u->nmenu; i++) {
        int x = bar_item_x(u, i, &w);
        if (mx >= x && mx < x + w) return i;
    }
    return -1;
}

/* ---- menu popup geometry -------------------------------------------------- */
static void popup_size(const uoc_look *k, const uoc_item *it, int n,
                       int *pw, int *ph)
{
    int i, w = 0, h = POPUP_BORDER * 2;
    for (i = 0; i < n; i++) {
        int iw;
        if (!it[i].text) { h += m_sep_h(); continue; }
        h += m_item_h(k);
        iw = m_gutter(k) + label_w(it[i].text) + k->pad * 2;
        if (accel_of(it[i].text))
            iw += m_accel_gap(k) + fb_text_w(accel_of(it[i].text));
        if (it[i].sub) iw += m_arrow_w(k);
        if (iw > w) w = iw;
    }
    *pw = w + POPUP_BORDER * 2;
    *ph = h;
}
static int popup_item_y(const uoc_look *k, const uoc_item *it, int n, int idx)
{
    int i, y = 0;
    for (i = 0; i < n && i < idx; i++)
        y += it[i].text ? m_item_h(k) : m_sep_h();
    return y;
}
static void popup_rect(const uoc_ui *u, int level, int *rx, int *ry,
                       int *rw, int *rh)
{
    const uoc_look *k = u->look;
    const uoc_item *it;
    int n, x, y, w, h;

    it = level_items(u, level, &n);
    popup_size(k, it, n, &w, &h);
    if (level == 0) {
        x = bar_item_x(u, u->open, 0);
        y = u->y + m_bar_h(k);
    } else {
        int px, py, pw, ph, pn;
        const uoc_item *pit;
        popup_rect(u, level - 1, &px, &py, &pw, &ph);
        pit = level_items(u, level - 1, &pn);
        y = py + popup_item_y(k, pit, pn, u->path[level - 1]);
        x = px + pw - POPUP_BORDER;
        if (x + w > FB_W) x = px - w + POPUP_BORDER;   /* flip to the left */
    }
    if (x + w > FB_W) x = FB_W - w;
    if (x < 0) x = 0;
    if (y + h > FB_H) y = FB_H - h;
    if (y < 0) y = 0;
    *rx = x; *ry = y; *rw = w; *rh = h;
}
static int popup_hit(const uoc_ui *u, int level, int mx, int my)
{
    const uoc_look *k = u->look;
    const uoc_item *it;
    int n, x, y, w, h, i, iy;
    it = level_items(u, level, &n);
    if (!it) return -1;
    popup_rect(u, level, &x, &y, &w, &h);
    if (mx < x || mx >= x + w || my < y || my >= y + h) return -1;
    iy = y + POPUP_BORDER;
    for (i = 0; i < n; i++) {
        int ih = it[i].text ? m_item_h(k) : m_sep_h();
        if (my >= iy && my < iy + ih) return i;
        iy += ih;
    }
    return -1;
}

/* ---- toolbar layout --------------------------------------------------------
 * One toolbar is a strip of `m_tb_h` thickness and a length that is the sum
 * of its items.  Docked, it sits in a BAND (a row at the top or bottom, a
 * column at the left or right); floating, it is a little window with a title
 * bar.  Painting, hit-testing, dragging and the drop preview all read their
 * coordinates from tb_origin() and tb_btn_rect() and from nowhere else. */
static int tb_vert(const uoc_ui *u, int b)
{ return u->bs[b].dock == UOC_DOCK_LEFT || u->bs[b].dock == UOC_DOCK_RIGHT; }

static int tb_item_len(const uoc_look *k, const uoc_tbitem *b, int vert)
{
    if (b->kind == UOC_TB_SEP)   return k->pad + 2;
    if (vert)                    return m_tb_btn(k);
    if (b->w > 0)                return b->w;
    if (b->kind == UOC_TB_COMBO) return k->icon_px * 5;
    if (b->kind == UOC_TB_SPLIT) return m_tb_btn(k) + k->pad + 7;
    return m_tb_btn(k);
}
static int tb_len(const uoc_ui *u, int b)
{
    const uoc_look *k = u->look;
    int i, n = 0, vert = tb_vert(u, b);
    for (i = 0; i < u->tbar[b].n; i++)
        n += tb_item_len(k, &u->tbar[b].item[i], vert);
    return n + (u->bs[b].dock == UOC_DOCK_FLOAT ? k->pad * 2 : m_grip_w(k) + 2);
}

static int tb_bands(const uoc_ui *u, int dock)
{
    int b, n = 0;
    for (b = 0; b < u->ntbar && b < UOC_MAXBARS; b++)
        if (!u->bs[b].hidden && u->bs[b].dock == dock && u->bs[b].band + 1 > n)
            n = u->bs[b].band + 1;
    return n;
}

static void tb_origin(const uoc_ui *u, int b, int *ox, int *oy)
{
    const uoc_look *k = u->look;
    int t = m_tb_h(k), i, off = 0;
    const uoc_barstate *s = &u->bs[b];

    if (s->dock == UOC_DOCK_FLOAT) {
        *ox = s->fx;
        *oy = s->fy + m_title_h(k);
        return;
    }
    /* bars sharing a band stack along it, in declaration order */
    for (i = 0; i < b; i++)
        if (!u->bs[i].hidden && u->bs[i].dock == s->dock &&
            u->bs[i].band == s->band)
            off += tb_len(u, i);

    switch (s->dock) {
    case UOC_DOCK_TOP:
        *ox = u->x + off;
        *oy = u->y + m_bar_h(k) + s->band * t;
        break;
    case UOC_DOCK_BOTTOM:
        *ox = u->x + off;
        *oy = u->y + u->h - (s->band + 1) * t;
        break;
    case UOC_DOCK_LEFT:
        *ox = u->x + s->band * t;
        *oy = u->y + m_bar_h(k) + tb_bands(u, UOC_DOCK_TOP) * t + off;
        break;
    default: /* RIGHT */
        *ox = u->x + u->w - (s->band + 1) * t;
        *oy = u->y + m_bar_h(k) + tb_bands(u, UOC_DOCK_TOP) * t + off;
        break;
    }
}

static int tb_btn_rect(const uoc_ui *u, int bar, int idx,
                       int *rx, int *ry, int *rw, int *rh)
{
    const uoc_look *k = u->look;
    int i, ox, oy, off, vert, grip;
    if (bar < 0 || bar >= u->ntbar || bar >= UOC_MAXBARS) return 0;
    if (idx < 0 || idx >= u->tbar[bar].n || u->bs[bar].hidden) return 0;
    vert = tb_vert(u, bar);
    grip = (u->bs[bar].dock == UOC_DOCK_FLOAT) ? k->pad : m_grip_w(k);
    tb_origin(u, bar, &ox, &oy);
    off = grip;
    for (i = 0; i < idx; i++) off += tb_item_len(k, &u->tbar[bar].item[i], vert);
    if (vert) {
        *rx = ox + 1;                       *ry = oy + off;
        *rw = m_tb_h(k) - 2;
        *rh = tb_item_len(k, &u->tbar[bar].item[idx], 1);
    } else {
        *rx = ox + off;                     *ry = oy + 1;
        *rw = tb_item_len(k, &u->tbar[bar].item[idx], 0);
        *rh = m_tb_h(k) - 2;
    }
    return 1;
}

/* The whole toolbar's rect, title bar included when floating. */
static void tb_rect(const uoc_ui *u, int b, int *rx, int *ry, int *rw, int *rh)
{
    const uoc_look *k = u->look;
    int ox, oy, len = tb_len(u, b), t = m_tb_h(k);
    tb_origin(u, b, &ox, &oy);
    if (u->bs[b].dock == UOC_DOCK_FLOAT) {
        *rx = u->bs[b].fx; *ry = u->bs[b].fy;
        *rw = len; *rh = t + m_title_h(k);
    } else if (tb_vert(u, b)) {
        *rx = ox; *ry = oy; *rw = t; *rh = len;
    } else {
        *rx = ox; *ry = oy; *rw = len; *rh = t;
    }
}

static int tb_hit(const uoc_ui *u, int mx, int my, int *bar)
{
    int b, i, x, y, w, h;
    *bar = -1;
    /* floating bars are on top, so they are tested first and in reverse */
    for (b = u->ntbar - 1; b >= 0; b--) {
        if (b >= UOC_MAXBARS || u->bs[b].hidden) continue;
        tb_rect(u, b, &x, &y, &w, &h);
        if (mx < x || mx >= x + w || my < y || my >= y + h) continue;
        *bar = b;
        for (i = 0; i < u->tbar[b].n; i++) {
            int bx, by, bw, bh;
            if (!tb_btn_rect(u, b, i, &bx, &by, &bw, &bh)) continue;
            if (u->tbar[b].item[i].kind == UOC_TB_SEP) continue;
            if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) return i;
        }
        return -1;
    }
    return -1;
}

/* The grab handle: the gripper when docked, the title bar when floating. */
static int tb_handle_hit(const uoc_ui *u, int mx, int my)
{
    const uoc_look *k = u->look;
    int b, x, y, w, h;
    for (b = u->ntbar - 1; b >= 0; b--) {
        if (b >= UOC_MAXBARS || u->bs[b].hidden) continue;
        tb_rect(u, b, &x, &y, &w, &h);
        if (u->bs[b].dock == UOC_DOCK_FLOAT) {
            if (mx >= x && mx < x + w && my >= y && my < y + m_title_h(k))
                return b;
        } else if (tb_vert(u, b)) {
            if (mx >= x && mx < x + w && my >= y && my < y + m_grip_w(k))
                return b;
        } else {
            if (mx >= x && mx < x + m_grip_w(k) && my >= y && my < y + h)
                return b;
        }
    }
    return -1;
}
static int tb_close_hit(const uoc_ui *u, int mx, int my)
{
    int b, x, y, w, h, s = fb_text_h();
    for (b = 0; b < u->ntbar && b < UOC_MAXBARS; b++) {
        if (u->bs[b].hidden || u->bs[b].dock != UOC_DOCK_FLOAT) continue;
        tb_rect(u, b, &x, &y, &w, &h);
        if (mx >= x + w - s - 2 && mx < x + w - 2 &&
            my >= y + 2 && my < y + 2 + s) return b;
    }
    return -1;
}

void uoc_dock(uoc_ui *u, int bar, int dock, int band, int fx, int fy)
{
    if (!u || bar < 0 || bar >= u->ntbar || bar >= UOC_MAXBARS) return;
    u->bs[bar].dock = dock;
    u->bs[bar].band = band < 0 ? 0 : band;
    if (dock == UOC_DOCK_FLOAT) { u->bs[bar].fx = fx; u->bs[bar].fy = fy; }
}

void uoc_client_rect(const uoc_ui *u, int *x, int *y, int *w, int *h)
{
    const uoc_look *k;
    int t, top, bot, lef, rig;
    if (!u || !u->look) return;
    k = u->look;
    t = m_tb_h(k);
    top = tb_bands(u, UOC_DOCK_TOP);
    bot = tb_bands(u, UOC_DOCK_BOTTOM);
    lef = tb_bands(u, UOC_DOCK_LEFT);
    rig = tb_bands(u, UOC_DOCK_RIGHT);
    if (x) *x = u->x + lef * t;
    if (y) *y = u->y + m_bar_h(k) + top * t;
    if (w) *w = u->w - (lef + rig) * t;
    if (h) *h = u->h - m_bar_h(k) - (top + bot) * t;
}

int uoc_height(const uoc_ui *u)
{
    if (!u || !u->look) return 0;
    return m_bar_h(u->look) + tb_bands(u, UOC_DOCK_TOP) * m_tb_h(u->look);
}

/* ---- drop-downs: the combo list and the palette ---------------------------- */
static void combo_rect(const uoc_ui *u, int bar, int idx,
                       int *rx, int *ry, int *rw, int *rh)
{
    const uoc_look *k = u->look;
    const uoc_tbitem *b = &u->tbar[bar].item[idx];
    int x, y, w, h, rows = b->nlist > 8 ? 8 : b->nlist;
    tb_btn_rect(u, bar, idx, &x, &y, &w, &h);
    *rx = x;
    *ry = y + h;
    *rw = w - k->pad;
    *rh = rows * (fb_text_h() + 2) + 2;
    if (*ry + *rh > FB_H) *ry = y - *rh;
}
static int combo_hit(const uoc_ui *u, int mx, int my)
{
    int x, y, w, h, row;
    combo_rect(u, u->pop_bar, u->pop_btn, &x, &y, &w, &h);
    if (mx < x || mx >= x + w || my < y || my >= y + h) return -1;
    row = (my - y - 1) / (fb_text_h() + 2);
    if (row < 0 || row >= u->tbar[u->pop_bar].item[u->pop_btn].nlist) return -1;
    return row;
}

static void pal_size(const uoc_look *k, const uoc_palette *p, int *w, int *h)
{
    int cols = p->cols > 0 ? p->cols : 4;
    int rows = (p->n + cols - 1) / cols;
    *w = cols * m_swatch(k) + 4;
    *h = rows * m_swatch(k) + 4 + TEARBAR_H;
}
static void pal_rect(const uoc_ui *u, int bar, int idx,
                     int *rx, int *ry, int *rw, int *rh)
{
    const uoc_look *k = u->look;
    const uoc_palette *p = u->tbar[bar].item[idx].pal;
    int x, y, w, h;
    tb_btn_rect(u, bar, idx, &x, &y, &w, &h);
    pal_size(k, p, rw, rh);
    *rx = x;
    *ry = y + h;
    if (*rx + *rw > FB_W) *rx = FB_W - *rw;
    if (*ry + *rh > FB_H) *ry = y - *rh;
}
static int pal_hit_at(const uoc_look *k, const uoc_palette *p,
                      int px, int py, int mx, int my)
{
    int cols = p->cols > 0 ? p->cols : 4, s = m_swatch(k), cx, cy, i, w, h;
    pal_size(k, p, &w, &h);
    if (mx < px || mx >= px + w || my < py + TEARBAR_H || my >= py + h) return -1;
    cx = (mx - px - 2) / s;
    cy = (my - py - TEARBAR_H - 2) / s;
    if (cx < 0 || cx >= cols || cy < 0) return -1;
    i = cy * cols + cx;
    return (i >= 0 && i < p->n) ? i : -1;
}

/* ---- public: setup and queries -------------------------------------------- */
void uoc_init(uoc_ui *u, const uoc_menu *menu, int nmenu,
              const uoc_tbar *tbar, int ntbar, int x, int y, int w, int h)
{
    int i;
    if (!u) return;
    for (i = 0; i < (int)sizeof *u; i++) ((char *)u)[i] = 0;
    u->look = uoc_look_97();
    u->menu = menu; u->nmenu = nmenu;
    u->tbar = tbar; u->ntbar = ntbar > UOC_MAXBARS ? UOC_MAXBARS : ntbar;
    u->x = x; u->y = y; u->w = w; u->h = h;
    u->open = -1; u->hot = -1;
    u->hot_bar = u->hot_btn = -1;
    u->down_bar = u->down_btn = -1;
    u->drag_bar = u->drag_tear = -1;
    u->tip_bar = u->tip_btn = -1;
    u->pop_pick = -1;
    for (i = 0; i < UOC_MAXDEPTH; i++) u->path[i] = -1;
    /* every toolbar starts docked at the top, one band each, in order */
    for (i = 0; i < u->ntbar; i++) {
        u->bs[i].dock = UOC_DOCK_TOP;
        u->bs[i].band = i;
    }
}

int uoc_menu_open(const uoc_ui *u) { return u && u->open >= 0; }
int uoc_pick(const uoc_ui *u) { return u ? u->pop_pick : -1; }

void uoc_dismiss(uoc_ui *u)
{
    int i;
    if (!u) return;
    u->open = -1; u->depth = 0; u->keyed = 0;
    u->pop_kind = UOC_POP_NONE;
    for (i = 0; i < UOC_MAXDEPTH; i++) u->path[i] = -1;
}

void uoc_toggle_set(uoc_ui *u, int id, int on)
{
    int i;
    if (!u || !id) return;
    for (i = 0; i < u->ntoggle; i++)
        if (u->toggle[i].id == id) { u->toggle[i].on = on; return; }
    if (u->ntoggle < UOC_MAXTOGGLE) {
        u->toggle[u->ntoggle].id = id;
        u->toggle[u->ntoggle].on = on;
        u->ntoggle++;
    }
}
int uoc_toggle(const uoc_ui *u, int id)
{
    int i;
    if (!u) return 0;
    for (i = 0; i < u->ntoggle; i++)
        if (u->toggle[i].id == id) return u->toggle[i].on;
    return 0;
}
void uoc_combo_set(uoc_ui *u, int id, int sel)
{
    int i;
    if (!u || !id) return;
    for (i = 0; i < u->ncombo; i++)
        if (u->combo[i].id == id) { u->combo[i].sel = sel; return; }
    if (u->ncombo < UOC_MAXTOGGLE) {
        u->combo[u->ncombo].id = id;
        u->combo[u->ncombo].sel = sel;
        u->ncombo++;
    }
}
int uoc_combo(const uoc_ui *u, int id)
{
    int i;
    if (!u) return 0;
    for (i = 0; i < u->ncombo; i++)
        if (u->combo[i].id == id) return u->combo[i].sel;
    return 0;
}

/* ---- painting ------------------------------------------------------------- */
static void paint_popup(const uoc_ui *u, int level)
{
    const uoc_look *k = u->look;
    const uoc_item *it;
    int n, x, y, w, h, i, iy, hot = (level < u->depth) ? u->path[level] : -1;

    it = level_items(u, level, &n);
    if (!it || !n) return;
    popup_rect(u, level, &x, &y, &w, &h);

    fb_fill_rect(x, y, w, h, k->face);
    raised(k, x, y, w, h);

    iy = y + POPUP_BORDER;
    for (i = 0; i < n; i++) {
        int ih, tx, ty, on = (i == hot);
        fb_px fg;
        if (!it[i].text) {
            ih = m_sep_h();
            etch_h(k, x + POPUP_BORDER + k->pad, iy + ih / 2 - 1,
                   w - 2 * (POPUP_BORDER + k->pad));
            iy += ih;
            continue;
        }
        ih = m_item_h(k);
        if (it[i].flags & UOC_DISABLED) on = 0;
        if (on) fb_fill_rect(x + POPUP_BORDER, iy, w - 2 * POPUP_BORDER, ih,
                             k->sel);
        fg = (it[i].flags & UOC_DISABLED) ? k->gray_text
                                          : (on ? k->sel_text : k->text);
        tx = x + POPUP_BORDER + m_gutter(k);
        ty = iy + (ih - fb_text_h()) / 2;

        if (it[i].icon >= 0) {
            int ix = x + POPUP_BORDER + 3, iyy = iy + (ih - k->icon_px) / 2;
            if (it[i].flags & UOC_CHECKED)
                sunken(k, ix - 1, iyy - 1, k->icon_px + 2, k->icon_px + 2);
            draw_icon(k, ix, iyy, it[i].icon,
                      (it[i].flags & UOC_DISABLED) != 0);
        } else if (it[i].flags & (UOC_CHECKED | UOC_RADIO)) {
            int ix = x + POPUP_BORDER + 3, iyy = iy + (ih - k->icon_px) / 2;
            if (it[i].flags & UOC_RADIO) draw_bullet(k, ix, iyy, fg);
            else                          draw_check(k, ix, iyy, fg);
        }

        /* Disabled text is the Win95 emboss: a white ghost down-right, then
         * the grey on top.  Plain grey alone reads as "low contrast bug". */
        if (it[i].flags & UOC_DISABLED)
            draw_label(tx + 1, ty + 1, it[i].text, k->hilight, 1);
        draw_label(tx, ty, it[i].text, fg, 1);

        if (accel_of(it[i].text)) {
            const char *a = accel_of(it[i].text);
            int aw = fb_text_w(a);
            int ax = x + w - POPUP_BORDER - k->pad - aw;
            if (it[i].sub) ax -= m_arrow_w(k);
            if (it[i].flags & UOC_DISABLED) fb_text(ax + 1, ty + 1, a, k->hilight, -1);
            fb_text(ax, ty, a, fg, -1);
        }
        if (it[i].sub)
            draw_arrow(x + w - POPUP_BORDER - m_arrow_w(k) + 2, iy, ih, fg);
        iy += ih;
    }
    if (level + 1 < u->depth) paint_popup(u, level + 1);
}

/* the grid a palette is, wherever it is drawn: dropped or torn off */
static void paint_palette_at(const uoc_look *k, const uoc_palette *p,
                             int x, int y, int hot, int tearbar)
{
    int cols = p->cols > 0 ? p->cols : 4, s = m_swatch(k), w, h, i;
    pal_size(k, p, &w, &h);
    fb_fill_rect(x, y, w, h, k->face);
    raised(k, x, y, w, h);
    if (tearbar) {
        /* the move bar: drag this and the palette tears off and floats */
        int bx = x + 3, by = y + 2, bw = w - 6;
        etch_h(k, bx, by, bw);
        etch_h(k, bx, by + 3, bw);
    }
    for (i = 0; i < p->n; i++) {
        int cx = x + 2 + (i % cols) * s, cy = y + 2 + TEARBAR_H + (i / cols) * s;
        if (i == hot) sunken(k, cx, cy, s, s);
        if (p->kind == UOC_PAL_COLOR) {
            fb_fill_rect(cx + 3, cy + 3, s - 6, s - 6, p->color[i]);
            fb_frame_rect(cx + 3, cy + 3, s - 6, s - 6, k->dkshadow);
        } else {
            draw_icon(k, cx + (s - k->icon_px) / 2, cy + (s - k->icon_px) / 2,
                      p->icon[i], 0);
        }
    }
}

static void paint_combo_list(const uoc_ui *u)
{
    const uoc_look *k = u->look;
    const uoc_tbitem *b = &u->tbar[u->pop_bar].item[u->pop_btn];
    int x, y, w, h, i, lh = fb_text_h() + 2, rows = b->nlist > 8 ? 8 : b->nlist;
    combo_rect(u, u->pop_bar, u->pop_btn, &x, &y, &w, &h);
    fb_fill_rect(x, y, w, h, k->hilight);
    fb_frame_rect(x, y, w, h, k->dkshadow);
    for (i = 0; i < rows; i++) {
        int ry = y + 1 + i * lh;
        fb_px fg = k->text;
        if (i == u->pop_hot) {
            fb_fill_rect(x + 1, ry, w - 2, lh, k->sel);
            fg = k->sel_text;
        }
        fb_text(x + 3, ry + 1, b->list[i], fg, -1);
    }
}

static void paint_toolbar(const uoc_ui *u, int bar)
{
    const uoc_look *k = u->look;
    int i, t = m_tb_h(k), vert = tb_vert(u, bar);
    int fx, fy, fw, fh;

    if (u->bs[bar].hidden) return;
    tb_rect(u, bar, &fx, &fy, &fw, &fh);

    if (u->bs[bar].dock == UOC_DOCK_FLOAT) {
        int th = m_title_h(k), s = fb_text_h();
        fb_fill_rect(fx, fy, fw, fh, k->face);
        raised(k, fx, fy, fw, fh);
        fb_fill_rect(fx + 2, fy + 2, fw - 4, th - 2, k->sel);
        fb_text(fx + 4, fy + 2, u->tbar[bar].name, k->sel_text, -1);
        fb_fill_rect(fx + fw - s - 2, fy + 2, s, s, k->face);
        raised(k, fx + fw - s - 2, fy + 2, s, s);
        fb_text(fx + fw - s + 1, fy + 3, "x", k->text, -1);
    } else {
        fb_fill_rect(fx, fy, fw, fh, k->face);
        if (vert) {          /* the gripper runs across the top of a column */
            etch_h(k, fx + 3, fy + 2, t - 6);
            etch_h(k, fx + 3, fy + 5, t - 6);
        } else {             /* ...and down the left of a row                */
            etch_v(k, fx + 2, fy + 3, t - 6);
            etch_v(k, fx + 5, fy + 3, t - 6);
        }
    }

    for (i = 0; i < u->tbar[bar].n; i++) {
        const uoc_tbitem *b = &u->tbar[bar].item[i];
        int x, y, w, h, on, hot, down;
        if (!tb_btn_rect(u, bar, i, &x, &y, &w, &h)) continue;
        if (b->kind == UOC_TB_SEP) {
            if (vert) etch_h(k, x + 2, y + h / 2 - 1, w - 4);
            else      etch_v(k, x + w / 2 - 1, y + 2, h - 4);
            continue;
        }

        hot  = (u->hot_bar == bar && u->hot_btn == i) && !(b->flags & UOC_DISABLED);
        down = (u->down_bar == bar && u->down_btn == i);
        on   = (b->kind == UOC_TB_TOGGLE) && uoc_toggle(u, b->id);
        if (u->pop_kind && u->pop_bar == bar && u->pop_btn == i) down = 1;

        if (b->kind == UOC_TB_COMBO) {
            int bw = k->icon_px, fw2 = w - k->pad - bw;
            const char *txt = b->tip;
            if (b->list && b->nlist) {
                int sel = uoc_combo(u, b->id);
                if (sel >= 0 && sel < b->nlist) txt = b->list[sel];
            }
            sunken(k, x, y, w - k->pad, h);
            fb_fill_rect(x + 2, y + 2, fw2 - 2, h - 4, k->hilight);
            if (txt) {
                char cut[64];
                fb_text(x + 4, y + (h - fb_text_h()) / 2,
                        fit_text(txt, fw2 - 6, cut, (int)sizeof cut),
                        k->text, -1);
            }
            fb_fill_rect(x + fw2, y + 2, bw - 2, h - 4, k->face);
            if (down) sunken(k, x + fw2, y + 2, bw - 2, h - 4);
            else      raised(k, x + fw2, y + 2, bw - 2, h - 4);
            draw_down(x + fw2 + bw / 2 - 4, y + h / 2 - 2, k->text);
            continue;
        }

        if (b->kind == UOC_TB_SPLIT) {
            int aw = k->pad + 7, mw = w - aw;
            if (down) sunken(k, x, y, mw, h);
            else if (hot) bevel(x, y, mw, h, k->hilight, k->shadow, 1);
            draw_icon(k, x + (mw - k->icon_px) / 2 + (down ? 1 : 0),
                      y + (h - k->icon_px) / 2 + (down ? 1 : 0),
                      b->icon, (b->flags & UOC_DISABLED) != 0);
            if (down) sunken(k, x + mw, y, aw, h);
            else if (hot) bevel(x + mw, y, aw, h, k->hilight, k->shadow, 1);
            draw_down(x + mw + aw / 2 - 4, y + h / 2 - 2,
                      (b->flags & UOC_DISABLED) ? k->gray_text : k->text);
            continue;
        }

        /* A toggled button sits on a 50% dither, which is how Office 97 says
         * "on" as opposed to merely "being pressed right now". */
        if (on && !down) {
            int px, py;
            for (py = y + 2; py < y + h - 2; py++)
                for (px = x + 2; px < x + w - 2; px++)
                    if (((px + py) & 1) == 0) fb_pixel(px, py, k->hilight);
        }
        if (down || on) sunken(k, x, y, w, h);
        else if (hot)   bevel(x, y, w, h, k->hilight, k->shadow, 1);

        {
            int ix = x + (w - k->icon_px) / 2, iy = y + (h - k->icon_px) / 2;
            if (down || on) { ix++; iy++; }
            draw_icon(k, ix, iy, b->icon, (b->flags & UOC_DISABLED) != 0);
        }
    }
}

static void paint_tip(const uoc_ui *u)
{
    const uoc_look *k = u->look;
    const uoc_tbitem *b;
    int x, y, w, h, tw;
    if (u->tip_bar < 0 || u->tip_btn < 0 || u->tip_ticks < UOC_TIP_TICKS) return;
    if (u->pop_kind || u->open >= 0 || u->drag_bar >= 0) return;
    if (u->tip_bar >= u->ntbar || u->tip_btn >= u->tbar[u->tip_bar].n) return;
    b = &u->tbar[u->tip_bar].item[u->tip_btn];
    if (!b->tip) return;
    if (!tb_btn_rect(u, u->tip_bar, u->tip_btn, &x, &y, &w, &h)) return;
    tw = fb_text_w(b->tip) + 6;
    x += w / 2;
    y += h + 2;
    if (x + tw > FB_W) x = FB_W - tw;
    fb_fill_rect(x, y, tw, fb_text_h() + 4, k->tip_bg);
    fb_frame_rect(x, y, tw, fb_text_h() + 4, k->dkshadow);
    fb_text(x + 3, y + 2, b->tip, k->text, -1);
}

void uoc_render(const uoc_ui *u)
{
    const uoc_look *k;
    int i, bh, b;
    if (!u || !u->look) return;
    k = u->look;
    bh = m_bar_h(k);

    fb_fill_rect(u->x, u->y, u->w, bh, k->face);
    for (i = 0; i < u->nmenu; i++) {
        int x, w, sel = (i == u->open) || (i == u->hot && u->open < 0);
        fb_px fg = sel ? k->sel_text : k->text;
        x = bar_item_x(u, i, &w);
        if (sel) fb_fill_rect(x, u->y + 1, w, bh - 2, k->sel);
        draw_label(x + k->pad + k->pad / 2, u->y + (bh - fb_text_h()) / 2,
                   u->menu[i].title, fg, 1);
    }
    /* docked bars first, then floating ones over the document */
    for (b = 0; b < u->ntbar; b++)
        if (u->bs[b].dock != UOC_DOCK_FLOAT) paint_toolbar(u, b);
    etch_h(k, u->x, u->y + uoc_height(u) - 2, u->w);
    for (b = 0; b < u->ntbar; b++)
        if (u->bs[b].dock == UOC_DOCK_FLOAT) paint_toolbar(u, b);

    /* torn-off palettes float above the bars but below an open menu */
    for (i = 0; i < UOC_MAXTEAR; i++) {
        const uoc_tearoff *t = &u->tear[i];
        int w, h, s = fb_text_h(), th = m_title_h(k);
        if (!t->open || !t->pal) continue;
        pal_size(k, t->pal, &w, &h);
        fb_fill_rect(t->x, t->y, w, th, k->face);
        raised(k, t->x, t->y, w, th + h);
        fb_fill_rect(t->x + 2, t->y + 2, w - 4, th - 2, k->sel);
        if (t->pal->title)
            fb_text(t->x + 4, t->y + 2, t->pal->title, k->sel_text, -1);
        fb_fill_rect(t->x + w - s - 2, t->y + 2, s, s, k->face);
        raised(k, t->x + w - s - 2, t->y + 2, s, s);
        fb_text(t->x + w - s + 1, t->y + 3, "x", k->text, -1);
        paint_palette_at(k, t->pal, t->x, t->y + th, -1, 0);
    }

    if (u->pop_kind == UOC_POP_COMBO) paint_combo_list(u);
    else if (u->pop_kind == UOC_POP_PALETTE) {
        int x, y, w, h;
        pal_rect(u, u->pop_bar, u->pop_btn, &x, &y, &w, &h);
        paint_palette_at(k, u->tbar[u->pop_bar].item[u->pop_btn].pal,
                         x, y, u->pop_hot, 1);
    }
    if (u->open >= 0 && u->depth > 0) paint_popup(u, 0);
    paint_tip(u);

    /* the drag preview goes over absolutely everything */
    if (u->drag_bar >= 0) {
        int w, h, len = tb_len(u, u->drag_bar);
        if (tb_vert(u, u->drag_bar)) { w = m_tb_h(k); h = len; }
        else                         { w = len; h = m_tb_h(k); }
        drag_outline(u->drag_x, u->drag_y, w, h, k->dkshadow);
    }
}

/* ---- events ---------------------------------------------------------------- */
static int item_selectable(const uoc_item *it)
{ return it && it->text && !(it->flags & UOC_DISABLED); }

static void move_hot(uoc_ui *u, int d)
{
    int n, lvl = u->depth - 1, i, cur;
    const uoc_item *it = level_items(u, lvl, &n);
    if (!it || n <= 0) return;
    cur = u->path[lvl];
    for (i = 0; i < n; i++) {
        cur += d;
        if (cur < 0) cur = n - 1;
        if (cur >= n) cur = 0;
        if (item_selectable(&it[cur])) { u->path[lvl] = cur; return; }
    }
}
static void open_menu(uoc_ui *u, int idx, int select_first)
{
    int n, i;
    const uoc_item *it;
    if (idx < 0 || idx >= u->nmenu) return;
    u->pop_kind = UOC_POP_NONE;
    u->open = idx;
    u->depth = 1;
    u->path[0] = -1;
    for (i = 1; i < UOC_MAXDEPTH; i++) u->path[i] = -1;
    if (select_first) {
        it = level_items(u, 0, &n);
        for (i = 0; i < n; i++)
            if (item_selectable(&it[i])) { u->path[0] = i; break; }
    }
}
static void open_sub(uoc_ui *u, int level, int select_first)
{
    int n, i;
    const uoc_item *it = level_items(u, level, &n);
    int idx = (level < UOC_MAXDEPTH) ? u->path[level] : -1;
    if (!it || idx < 0 || idx >= n || !it[idx].sub) return;
    if (level + 1 >= UOC_MAXDEPTH) return;
    u->depth = level + 2;
    u->path[level + 1] = -1;
    if (select_first) {
        const uoc_item *sub = level_items(u, level + 1, &n);
        for (i = 0; i < n; i++)
            if (item_selectable(&sub[i])) { u->path[level + 1] = i; break; }
    }
}
static int activate(uoc_ui *u)
{
    int n, lvl = u->depth - 1;
    const uoc_item *it = level_items(u, lvl, &n);
    int idx = (lvl >= 0 && lvl < UOC_MAXDEPTH) ? u->path[lvl] : -1;
    if (!it || idx < 0 || idx >= n) return 0;
    if (!item_selectable(&it[idx])) return 0;
    if (it[idx].sub) { open_sub(u, lvl, 1); return 0; }
    { int id = it[idx].id; uoc_dismiss(u); return id; }
}

/* Tear a palette off its button and leave it floating at (x,y).  Returns the
 * slot it landed in so the caller can start dragging it, or -1. */
static int tear_off(uoc_ui *u, int x, int y)
{
    const uoc_tbitem *b = &u->tbar[u->pop_bar].item[u->pop_btn];
    int i;
    u->pop_kind = UOC_POP_NONE;
    for (i = 0; i < UOC_MAXTEAR; i++)
        if (!u->tear[i].open) {
            u->tear[i].pal = b->pal;
            u->tear[i].x = x; u->tear[i].y = y;
            u->tear[i].id = b->id;
            u->tear[i].open = 1;
            return i;
        }
    return -1;
}

/* Where should a dragged toolbar land?  Within a bar and a half of a frame
 * edge it docks there, appended as a new band; anywhere else it floats. */
static void drop_bar(uoc_ui *u, int b, int mx, int my)
{
    const uoc_look *k = u->look;
    int m = DOCK_MARGIN(k);
    int dtop = my - (u->y + m_bar_h(k)), dbot = (u->y + u->h) - my;
    int dlef = mx - u->x,                drig = (u->x + u->w) - mx;

    if (dtop >= 0 && dtop < m)
        uoc_dock(u, b, UOC_DOCK_TOP, tb_bands(u, UOC_DOCK_TOP), 0, 0);
    else if (dbot >= 0 && dbot < m)
        uoc_dock(u, b, UOC_DOCK_BOTTOM, tb_bands(u, UOC_DOCK_BOTTOM), 0, 0);
    else if (dlef >= 0 && dlef < m)
        uoc_dock(u, b, UOC_DOCK_LEFT, tb_bands(u, UOC_DOCK_LEFT), 0, 0);
    else if (drig >= 0 && drig < m)
        uoc_dock(u, b, UOC_DOCK_RIGHT, tb_bands(u, UOC_DOCK_RIGHT), 0, 0);
    else
        uoc_dock(u, b, UOC_DOCK_FLOAT, 0, mx - u->drag_dx, my - u->drag_dy);
}

static int handle_mouse(uoc_ui *u, const unoui_event *e, int *cmd)
{
    const uoc_look *k = u->look;
    int lvl, hitbar, bar, btn, i;

    /* ---- a drag in progress owns every event until the button comes up */
    if (u->drag_bar >= 0) {
        if (e->kind == UI_EV_MOUSE_MOVE) {
            u->drag_x = e->x - u->drag_dx;
            u->drag_y = e->y - u->drag_dy;
            return 1;
        }
        if (e->kind == UI_EV_MOUSE_UP) {
            drop_bar(u, u->drag_bar, e->x, e->y);
            u->drag_bar = -1;
            return 1;
        }
    }
    if (u->drag_tear >= 0) {
        if (e->kind == UI_EV_MOUSE_MOVE) {
            u->tear[u->drag_tear].x = e->x - u->drag_dx;
            u->tear[u->drag_tear].y = e->y - u->drag_dy;
            return 1;
        }
        if (e->kind == UI_EV_MOUSE_UP) { u->drag_tear = -1; return 1; }
    }

    if (e->kind == UI_EV_MOUSE_MOVE) {
        if (u->pop_kind == UOC_POP_COMBO) {
            u->pop_hot = combo_hit(u, e->x, e->y);
            return 1;
        }
        if (u->pop_kind == UOC_POP_PALETTE) {
            int x, y, w, h;
            pal_rect(u, u->pop_bar, u->pop_btn, &x, &y, &w, &h);
            u->pop_hot = pal_hit_at(k, u->tbar[u->pop_bar].item[u->pop_btn].pal,
                                    x, y, e->x, e->y);
            return 1;
        }
        if (u->open >= 0) {
            int b = bar_hit(u, e->x, e->y);
            if (b >= 0 && b != u->open) { open_menu(u, b, 0); return 1; }
            for (lvl = u->depth - 1; lvl >= 0; lvl--) {
                int idx = popup_hit(u, lvl, e->x, e->y);
                if (idx < 0) continue;
                u->depth = lvl + 1;
                u->path[lvl] = idx;
                {   int n;
                    const uoc_item *it = level_items(u, lvl, &n);
                    if (it && idx < n && !item_selectable(&it[idx])) u->path[lvl] = -1;
                    else if (it && idx < n && it[idx].sub) open_sub(u, lvl, 0);
                }
                return 1;
            }
            return 1;
        }
        u->hot = bar_hit(u, e->x, e->y);
        btn = tb_hit(u, e->x, e->y, &bar);
        if (bar != u->hot_bar || btn != u->hot_btn) {
            u->tip_bar = bar; u->tip_btn = btn; u->tip_ticks = 0;
        }
        u->hot_bar = bar; u->hot_btn = btn;
        return (u->hot >= 0 || btn >= 0 || bar >= 0);
    }

    if (e->kind == UI_EV_MOUSE_DOWN) {
        /* an open palette: the move bar tears it off, a swatch picks */
        if (u->pop_kind == UOC_POP_PALETTE) {
            int x, y, w, h, pick;
            pal_rect(u, u->pop_bar, u->pop_btn, &x, &y, &w, &h);
            if (e->x >= x && e->x < x + w && e->y >= y && e->y < y + TEARBAR_H) {
                int slot;
                u->drag_dx = e->x - x; u->drag_dy = e->y - y;
                slot = tear_off(u, x, y);
                if (slot >= 0) u->drag_tear = slot;
                return 1;
            }
            pick = pal_hit_at(k, u->tbar[u->pop_bar].item[u->pop_btn].pal,
                              x, y, e->x, e->y);
            if (pick >= 0) {
                u->pop_pick = pick;
                *cmd = u->tbar[u->pop_bar].item[u->pop_btn].id;
            }
            u->pop_kind = UOC_POP_NONE;
            return 1;
        }
        if (u->pop_kind == UOC_POP_COMBO) {
            int pick = combo_hit(u, e->x, e->y);
            if (pick >= 0) {
                const uoc_tbitem *b = &u->tbar[u->pop_bar].item[u->pop_btn];
                u->pop_pick = pick;
                uoc_combo_set(u, b->id, pick);
                *cmd = b->id;
            }
            u->pop_kind = UOC_POP_NONE;
            return 1;
        }
        /* a torn-off palette: its title bar drags, its X closes, a swatch
         * picks - it behaves exactly like the dropped one it came from */
        for (i = UOC_MAXTEAR - 1; i >= 0; i--) {
            uoc_tearoff *t = &u->tear[i];
            int w, h, th = m_title_h(k), s = fb_text_h(), sw;
            if (!t->open || !t->pal) continue;
            pal_size(k, t->pal, &w, &h);
            if (e->x < t->x || e->x >= t->x + w ||
                e->y < t->y || e->y >= t->y + th + h) continue;
            if (e->y < t->y + th) {
                if (e->x >= t->x + w - s - 2 && e->x < t->x + w - 2)
                    t->open = 0;
                else {
                    u->drag_tear = i;
                    u->drag_dx = e->x - t->x; u->drag_dy = e->y - t->y;
                }
                return 1;
            }
            sw = pal_hit_at(k, t->pal, t->x, t->y + th, e->x, e->y);
            if (sw >= 0) { u->pop_pick = sw; *cmd = t->id; }
            return 1;
        }

        hitbar = bar_hit(u, e->x, e->y);
        if (hitbar >= 0) {
            if (u->open == hitbar) uoc_dismiss(u);
            else                   open_menu(u, hitbar, 0);
            return 1;
        }
        if (u->open >= 0) {
            for (lvl = u->depth - 1; lvl >= 0; lvl--)
                if (popup_hit(u, lvl, e->x, e->y) >= 0) return 1;
            uoc_dismiss(u);
            return 1;
        }
        /* a floating bar's close box, then any bar's handle, then buttons */
        i = tb_close_hit(u, e->x, e->y);
        if (i >= 0) { u->bs[i].hidden = 1; return 1; }
        i = tb_handle_hit(u, e->x, e->y);
        if (i >= 0) {
            int rx, ry, rw, rh;
            tb_rect(u, i, &rx, &ry, &rw, &rh);
            u->drag_bar = i;
            u->drag_dx = e->x - rx; u->drag_dy = e->y - ry;
            u->drag_x = rx; u->drag_y = ry;
            return 1;
        }
        btn = tb_hit(u, e->x, e->y, &bar);
        if (btn >= 0 && !(u->tbar[bar].item[btn].flags & UOC_DISABLED)) {
            const uoc_tbitem *b = &u->tbar[bar].item[btn];
            int bx, by, bw, bh;
            tb_btn_rect(u, bar, btn, &bx, &by, &bw, &bh);
            if (b->kind == UOC_TB_COMBO && b->list && b->nlist) {
                u->pop_kind = UOC_POP_COMBO;
                u->pop_bar = bar; u->pop_btn = btn;
                u->pop_hot = uoc_combo(u, b->id);
                return 1;
            }
            if (b->kind == UOC_TB_SPLIT && b->pal &&
                e->x >= bx + bw - (k->pad + 7)) {
                u->pop_kind = UOC_POP_PALETTE;
                u->pop_bar = bar; u->pop_btn = btn;
                u->pop_hot = -1;
                return 1;
            }
            u->down_bar = bar; u->down_btn = btn;
            return 1;
        }
        return bar >= 0;
    }

    if (e->kind == UI_EV_MOUSE_UP) {
        if (u->down_btn >= 0) {
            int b = u->down_bar, idx = u->down_btn;
            int was = (tb_hit(u, e->x, e->y, &bar) == idx && bar == b);
            u->down_bar = u->down_btn = -1;
            if (was) {
                const uoc_tbitem *it = &u->tbar[b].item[idx];
                if (it->kind == UOC_TB_TOGGLE)
                    uoc_toggle_set(u, it->id, !uoc_toggle(u, it->id));
                *cmd = it->id;
            }
            return 1;
        }
        if (u->open >= 0) {
            if (bar_hit(u, e->x, e->y) >= 0) return 1;
            for (lvl = u->depth - 1; lvl >= 0; lvl--) {
                int idx = popup_hit(u, lvl, e->x, e->y);
                if (idx < 0) continue;
                u->depth = lvl + 1;
                u->path[lvl] = idx;
                *cmd = activate(u);
                return 1;
            }
            return 1;
        }
    }
    return 0;
}

static int handle_key(uoc_ui *u, const unoui_event *e, int *cmd)
{
    int key = e->key;

    if (key == UOC_KEY_F10) {
        if (u->open >= 0 || u->keyed) uoc_dismiss(u);
        else { u->keyed = 1; u->hot = 0; u->open = -1; }
        return 1;
    }
    if (u->pop_kind && key == UI_KEY_ESC) { u->pop_kind = UOC_POP_NONE; return 1; }
    if (u->open < 0 && !u->keyed) return 0;

    switch (key) {
    case UI_KEY_ESC:
        if (u->depth > 1) u->depth--;
        else if (u->open >= 0) { u->open = -1; u->depth = 0; u->keyed = 1; }
        else u->keyed = 0;
        return 1;
    case UI_KEY_LEFT:
        if (u->depth > 1) { u->depth--; return 1; }
        {   int b = (u->open >= 0 ? u->open : u->hot) - 1;
            if (b < 0) b = u->nmenu - 1;
            if (u->open >= 0) open_menu(u, b, 1); else u->hot = b;
        }
        return 1;
    case UI_KEY_RIGHT:
        if (u->open >= 0 && u->depth > 0) {
            int n;
            const uoc_item *it = level_items(u, u->depth - 1, &n);
            int idx = u->path[u->depth - 1];
            if (it && idx >= 0 && idx < n && it[idx].sub) {
                open_sub(u, u->depth - 1, 1);
                return 1;
            }
        }
        {   int b = (u->open >= 0 ? u->open : u->hot) + 1;
            if (b >= u->nmenu) b = 0;
            if (u->open >= 0) open_menu(u, b, 1); else u->hot = b;
        }
        return 1;
    case UI_KEY_DOWN:
        if (u->open < 0) { open_menu(u, u->hot < 0 ? 0 : u->hot, 1); return 1; }
        move_hot(u, 1);
        return 1;
    case UI_KEY_UP:
        if (u->open < 0) { open_menu(u, u->hot < 0 ? 0 : u->hot, 1); return 1; }
        move_hot(u, -1);
        return 1;
    case UI_KEY_ENTER:
        if (u->open < 0) { open_menu(u, u->hot < 0 ? 0 : u->hot, 1); return 1; }
        *cmd = activate(u);
        return 1;
    default: break;
    }
    return 0;
}

static int handle_char(uoc_ui *u, const unoui_event *e, int *cmd)
{
    int c = e->ch, i, n;
    const uoc_item *it;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < ' ') return 0;

    if (u->open < 0) {
        if (!(e->mods & UI_MOD_ALT) && !u->keyed) return 0;
        for (i = 0; i < u->nmenu; i++)
            if (uoc_mnemonic_of(u->menu[i].title) == c) {
                open_menu(u, i, 1);
                return 1;
            }
        return u->keyed;
    }
    it = level_items(u, u->depth - 1, &n);
    for (i = 0; i < n; i++)
        if (item_selectable(&it[i]) && uoc_mnemonic_of(it[i].text) == c) {
            u->path[u->depth - 1] = i;
            *cmd = activate(u);
            return 1;
        }
    return 1;
}

int uoc_handle(uoc_ui *u, const unoui_event *e, int *cmd)
{
    int dummy = 0;
    if (!cmd) cmd = &dummy;
    *cmd = 0;
    if (!u || !u->look || !e) return 0;
    switch (e->kind) {
    case UI_EV_MOUSE_DOWN:
    case UI_EV_MOUSE_UP:
    case UI_EV_MOUSE_MOVE: return handle_mouse(u, e, cmd);
    case UI_EV_KEY:        return handle_key(u, e, cmd);
    case UI_EV_CHAR:       return handle_char(u, e, cmd);
    case UI_EV_TICK:
        /* the ScreenTip's dwell timer, capped so it cannot run away */
        if (u->tip_btn >= 0 && u->tip_ticks < UOC_TIP_TICKS * 4) u->tip_ticks++;
        return 0;
    default:               return 0;
    }
}

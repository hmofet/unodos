/* ===========================================================================
 * uochrome.c - the Office 97 command-bar engine (OFFICE97-PLAN §5 phase 6a).
 *
 * See uochrome.h for what this is and why it is not unoui.  Three things in
 * here are worth knowing before changing any of it:
 *
 *   1. GEOMETRY IS COMPUTED ONCE, by the same functions the painter and the
 *      hit-tester both call.  unoui learned this the expensive way (its
 *      unoui_content_origin comment says so): if a click is tested against
 *      arithmetic that only resembles what was drawn, the two drift and
 *      buttons stop working where they look.
 *   2. METRICS DERIVE FROM THE FONT, never from a pixel count.  On the host
 *      harness fb_text is an 8x8 bitmap; on pc64 it is the kerned TTF
 *      engine at whatever size and UI scale the user picked.  Both must
 *      lay out correctly, so everything is fb_text_h() / fb_text_w() plus
 *      the look table's paddings.
 *   3. EVERY GESTURE IS A PURE FUNCTION OF THE EVENT STREAM.  That is what
 *      makes the host storyboard gate meaningful: it drives the same
 *      uoc_handle() the OS drives.
 * ======================================================================== */
#include "uochrome.h"

/* Freestanding: pc64 links no host libc into this lane. */
static int u_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

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
#define POPUP_BORDER 2

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
/* The mnemonic letter, uppercased, or 0. */
static int mnemonic_of(const char *s)
{
    char buf[96];
    int mn, c;
    label_of(s, buf, (int)sizeof buf, &mn);
    if (mn < 0) return 0;
    c = (unsigned char)buf[mn];
    if (c >= 'a' && c <= 'z') c -= 32;
    return c;
}

/* Draw a label, underlining its mnemonic.  Returns the width drawn. */
static int draw_label(int x, int y, const char *s, fb_px fg, int show_mn)
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
static int label_w(const char *s)
{
    char buf[96]; int mn;
    label_of(s, buf, (int)sizeof buf, &mn);
    return fb_text_w(buf);
}

/* ---- bevels ----------------------------------------------------------------
 * The two Windows 95 edges everything here is made of.  `thick` 1 is the
 * "inner" edge a hovered toolbar button gets; 2 is the full raised edge of a
 * popup or a pressed control. */
static void bevel(int x, int y, int w, int h, fb_px tl, fb_px br, int thick)
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
static void raised(const uoc_look *k, int x, int y, int w, int h)
{
    bevel(x, y, w, h, k->light, k->dkshadow, 1);
    bevel(x + 1, y + 1, w - 2, h - 2, k->hilight, k->shadow, 1);
}
static void sunken(const uoc_look *k, int x, int y, int w, int h)
{
    bevel(x, y, w, h, k->shadow, k->hilight, 1);
    bevel(x + 1, y + 1, w - 2, h - 2, k->dkshadow, k->light, 1);
}
/* The etched line a separator is: dark above, bright below. */
static void etch_h(const uoc_look *k, int x, int y, int w)
{ fb_hline(x, y, w, k->shadow); fb_hline(x, y + 1, w, k->hilight); }
static void etch_v(const uoc_look *k, int x, int y, int h)
{ fb_vline(x, y, h, k->shadow); fb_vline(x + 1, y, h, k->hilight); }

/* ---- icons -----------------------------------------------------------------
 * The atlas is an installation seam (phase 6b brings artwork).  Until then an
 * icon draws as a deterministic placeholder derived from its index, so the
 * storyboard still proves the index is plumbed through to the right cell. */
static const fb_px *g_atlas;
static int g_cell, g_cols, g_count;

void uoc_set_icons(const fb_px *atlas, int cell, int cols, int count)
{ g_atlas = atlas; g_cell = cell; g_cols = cols; g_count = count; }

static void draw_icon(const uoc_look *k, int x, int y, int idx, int disabled)
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
    {   /* placeholder: a framed 3x3 pattern hashed from the index */
        unsigned h = (unsigned)idx * 2654435761u;
        int r, c, cellw = (p - 4) / 3;
        fb_px on = disabled ? k->gray_text : k->dkshadow;
        fb_frame_rect(x, y, p, p, disabled ? k->gray_text : k->shadow);
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++, h >>= 1)
                if (h & 1)
                    fb_fill_rect(x + 2 + c * cellw, y + 2 + r * cellw,
                                 cellw, cellw, on);
    }
}

/* A check mark and a radio bullet, for the icon gutter. */
static void draw_check(const uoc_look *k, int x, int y, fb_px c)
{
    int p = k->icon_px, i, n = p / 3;
    (void)k;
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

/* The right-pointing triangle on a submenu parent. */
static void draw_arrow(int x, int y, int h, fb_px c)
{
    int i, n = 4;
    for (i = 0; i < n; i++)
        fb_vline(x + i, y + h / 2 - (n - 1 - i), (n - i) * 2 - 1, c);
}
/* The down triangle on a combo's drop button. */
static void draw_down(int x, int y, fb_px c)
{
    int i, n = 4;
    for (i = 0; i < n; i++)
        fb_hline(x + i, y + i, (n - i) * 2 - 1, c);
}

/* ---- the item lists at each open level -------------------------------------
 * Level 0 is the open menu's own items; level k is the submenu hanging off
 * the item path[k-1] chose.  Everything else derives from this walk, which is
 * why it is one function rather than a cached array that could go stale. */
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

/* ---- popup geometry ------------------------------------------------------- */
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

/* The y of item `idx` inside a popup whose body starts at py. */
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
        y = py + POPUP_BORDER +
            popup_item_y(k, pit, pn, u->path[level - 1]) - POPUP_BORDER;
        x = px + pw - POPUP_BORDER;
        if (x + w > FB_W) x = px - w + POPUP_BORDER;   /* flip to the left */
    }
    if (x + w > FB_W) x = FB_W - w;
    if (x < 0) x = 0;
    if (y + h > FB_H) y = FB_H - h;
    if (y < 0) y = 0;
    *rx = x; *ry = y; *rw = w; *rh = h;
}

/* Which item of the level-`level` popup is at (mx,my)?  -1 for none, and
 * separators report themselves so a drag over one does not "fall through" to
 * the item beyond. */
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

/* ---- toolbars ------------------------------------------------------------- */
static int tb_natural_w(const uoc_look *k, const uoc_tbitem *b)
{
    if (b->w > 0) return b->w;
    switch (b->kind) {
    case UOC_TB_SEP:   return k->pad + 2;
    case UOC_TB_COMBO: return k->icon_px * 5;
    case UOC_TB_SPLIT: return m_tb_btn(k) + k->pad + 7;
    default:           return m_tb_btn(k);
    }
}
static int tb_row_y(const uoc_ui *u, int bar)
{ return u->y + m_bar_h(u->look) + bar * m_tb_h(u->look); }

static int tb_btn_rect(const uoc_ui *u, int bar, int idx,
                       int *rx, int *ry, int *rw, int *rh)
{
    const uoc_look *k = u->look;
    int i, x;
    if (bar < 0 || bar >= u->ntbar || idx < 0 || idx >= u->tbar[bar].n) return 0;
    x = u->x + m_grip_w(k);
    for (i = 0; i < idx; i++) x += tb_natural_w(k, &u->tbar[bar].item[i]);
    *rx = x;
    *ry = tb_row_y(u, bar) + 1;
    *rw = tb_natural_w(k, &u->tbar[bar].item[idx]);
    *rh = m_tb_h(k) - 2;
    return 1;
}
static int tb_hit(const uoc_ui *u, int mx, int my, int *bar)
{
    int b, i, x, y, w, h;
    for (b = 0; b < u->ntbar; b++) {
        int ry = tb_row_y(u, b);
        if (my < ry || my >= ry + m_tb_h(u->look)) continue;
        for (i = 0; i < u->tbar[b].n; i++) {
            if (!tb_btn_rect(u, b, i, &x, &y, &w, &h)) continue;
            if (u->tbar[b].item[i].kind == UOC_TB_SEP) continue;
            if (mx >= x && mx < x + w) { *bar = b; return i; }
        }
        *bar = b;
        return -1;
    }
    *bar = -1;
    return -1;
}

/* ---- public: setup and queries -------------------------------------------- */
void uoc_init(uoc_ui *u, const uoc_menu *menu, int nmenu,
              const uoc_tbar *tbar, int ntbar, int x, int y, int w)
{
    int i;
    if (!u) return;
    u->look = uoc_look_97();
    u->menu = menu; u->nmenu = nmenu;
    u->tbar = tbar; u->ntbar = ntbar;
    u->x = x; u->y = y; u->w = w;
    u->open = -1; u->hot = -1; u->depth = 0; u->keyed = 0;
    u->hot_bar = u->hot_btn = -1;
    u->down_bar = u->down_btn = -1;
    u->ntoggle = 0;
    for (i = 0; i < UOC_MAXDEPTH; i++) u->path[i] = -1;
}

int uoc_height(const uoc_ui *u)
{ return u ? m_bar_h(u->look) + u->ntbar * m_tb_h(u->look) : 0; }

int uoc_menu_open(const uoc_ui *u) { return u && u->open >= 0; }

void uoc_dismiss(uoc_ui *u)
{
    int i;
    if (!u) return;
    u->open = -1; u->depth = 0; u->keyed = 0;
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
        if (!it[i].text) {                       /* a separator */
            ih = m_sep_h();
            etch_h(k, x + POPUP_BORDER + k->pad, iy + ih / 2 - 1,
                   w - 2 * (POPUP_BORDER + k->pad));
            iy += ih;
            continue;
        }
        ih = m_item_h(k);
        if (it[i].flags & UOC_DISABLED) on = 0;   /* disabled never highlights */
        if (on) fb_fill_rect(x + POPUP_BORDER, iy, w - 2 * POPUP_BORDER, ih,
                             k->sel);
        fg = (it[i].flags & UOC_DISABLED) ? k->gray_text
                                          : (on ? k->sel_text : k->text);
        tx = x + POPUP_BORDER + m_gutter(k);
        ty = iy + (ih - fb_text_h()) / 2;

        /* the icon gutter: an icon, or a check/bullet when there is none */
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
    /* Submenus paint on top of their parent, deepest last. */
    if (level + 1 < u->depth) paint_popup(u, level + 1);
}

static void paint_toolbar(const uoc_ui *u, int bar)
{
    const uoc_look *k = u->look;
    int ry = tb_row_y(u, bar), rh = m_tb_h(k), i;

    fb_fill_rect(u->x, ry, u->w, rh, k->face);
    /* the gripper: the two raised bars at a docked toolbar's left end */
    etch_v(k, u->x + 2, ry + 3, rh - 6);
    etch_v(k, u->x + 5, ry + 3, rh - 6);

    for (i = 0; i < u->tbar[bar].n; i++) {
        const uoc_tbitem *b = &u->tbar[bar].item[i];
        int x, y, w, h, on, hot, down;
        if (!tb_btn_rect(u, bar, i, &x, &y, &w, &h)) continue;
        if (b->kind == UOC_TB_SEP) { etch_v(k, x + w / 2 - 1, y + 2, h - 4); continue; }

        hot  = (u->hot_bar == bar && u->hot_btn == i) && !(b->flags & UOC_DISABLED);
        down = (u->down_bar == bar && u->down_btn == i);
        on   = (b->kind == UOC_TB_TOGGLE) && uoc_toggle(u, b->id);

        if (b->kind == UOC_TB_COMBO) {
            /* A Windows 95 combo: a sunken white field with a raised drop
             * button at its right end.  Phase 6a draws it and reserves its
             * space; 6b makes the button open a list. */
            int bw = k->icon_px, fw = w - k->pad - bw;
            sunken(k, x, y, w - k->pad, h);
            fb_fill_rect(x + 2, y + 2, fw - 2, h - 4, k->hilight);
            if (b->tip) fb_text(x + 4, y + (h - fb_text_h()) / 2, b->tip,
                                k->text, -1);
            fb_fill_rect(x + fw, y + 2, bw - 2, h - 4, k->face);
            raised(k, x + fw, y + 2, bw - 2, h - 4);
            draw_down(x + fw + bw / 2 - 4, y + h / 2 - 2, k->text);
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
            if (down || on) { ix++; iy++; }      /* pressed art shifts 1,1 */
            draw_icon(k, ix, iy, b->icon, (b->flags & UOC_DISABLED) != 0);
        }
    }
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
    for (b = 0; b < u->ntbar; b++) paint_toolbar(u, b);
    /* the bar block's bottom edge, then anything open on top of everything */
    etch_h(k, u->x, u->y + uoc_height(u) - 2, u->w);
    if (u->open >= 0 && u->depth > 0) paint_popup(u, 0);
}

/* ---- events ---------------------------------------------------------------- */
static int item_selectable(const uoc_item *it)
{ return it && it->text && !(it->flags & UOC_DISABLED); }

/* Move the hot item at the deepest level by `d`, skipping separators and
 * disabled items, wrapping at the ends as Windows menus do. */
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

/* Open the submenu of the hot item at `level`, if it has one. */
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

/* Activate whatever is hot.  Returns the command id, or 0. */
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

static int handle_mouse(uoc_ui *u, const unoui_event *e, int *cmd)
{
    int lvl, hitbar, bar, btn;

    if (e->kind == UI_EV_MOUSE_MOVE) {
        if (u->open >= 0) {
            /* Track across the menu bar while open, as Windows does. */
            int b = bar_hit(u, e->x, e->y);
            if (b >= 0 && b != u->open) { open_menu(u, b, 0); return 1; }
            for (lvl = u->depth - 1; lvl >= 0; lvl--) {
                int i = popup_hit(u, lvl, e->x, e->y);
                if (i < 0) continue;
                u->depth = lvl + 1;
                u->path[lvl] = i;
                {   int n;
                    const uoc_item *it = level_items(u, lvl, &n);
                    if (it && i < n && !item_selectable(&it[i])) u->path[lvl] = -1;
                    else if (it && i < n && it[i].sub) open_sub(u, lvl, 0);
                }
                return 1;
            }
            return 1;                       /* inside an open menu, eat it   */
        }
        u->hot = bar_hit(u, e->x, e->y);
        btn = tb_hit(u, e->x, e->y, &bar);
        u->hot_bar = bar; u->hot_btn = btn;
        return (u->hot >= 0 || btn >= 0);
    }

    if (e->kind == UI_EV_MOUSE_DOWN) {
        hitbar = bar_hit(u, e->x, e->y);
        if (hitbar >= 0) {
            if (u->open == hitbar) uoc_dismiss(u);
            else                   open_menu(u, hitbar, 0);
            return 1;
        }
        if (u->open >= 0) {
            for (lvl = u->depth - 1; lvl >= 0; lvl--)
                if (popup_hit(u, lvl, e->x, e->y) >= 0) return 1;
            uoc_dismiss(u);                 /* a click outside closes it     */
            return 1;
        }
        btn = tb_hit(u, e->x, e->y, &bar);
        if (btn >= 0 && !(u->tbar[bar].item[btn].flags & UOC_DISABLED)) {
            u->down_bar = bar; u->down_btn = btn;
            return 1;
        }
        return bar >= 0;                    /* the bar itself swallows it    */
    }

    if (e->kind == UI_EV_MOUSE_UP) {
        if (u->down_btn >= 0) {
            int b = u->down_bar, i = u->down_btn;
            int was = (tb_hit(u, e->x, e->y, &bar) == i && bar == b);
            u->down_bar = u->down_btn = -1;
            if (was) {
                const uoc_tbitem *it = &u->tbar[b].item[i];
                if (it->kind == UOC_TB_TOGGLE)
                    uoc_toggle_set(u, it->id, !uoc_toggle(u, it->id));
                *cmd = it->id;
            }
            return 1;
        }
        if (u->open >= 0) {
            if (bar_hit(u, e->x, e->y) >= 0) return 1;   /* stays open       */
            for (lvl = u->depth - 1; lvl >= 0; lvl--) {
                int i = popup_hit(u, lvl, e->x, e->y);
                if (i < 0) continue;
                u->depth = lvl + 1;
                u->path[lvl] = i;
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

/* A typed letter: the mnemonic of a top-level menu (with Alt, or while the
 * bar is keyed) or of an item in the open popup. */
static int handle_char(uoc_ui *u, const unoui_event *e, int *cmd)
{
    int c = e->ch, i, n;
    const uoc_item *it;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < ' ') return 0;

    if (u->open < 0) {
        if (!(e->mods & UI_MOD_ALT) && !u->keyed) return 0;
        for (i = 0; i < u->nmenu; i++)
            if (mnemonic_of(u->menu[i].title) == c) {
                open_menu(u, i, 1);
                return 1;
            }
        return u->keyed;
    }
    it = level_items(u, u->depth - 1, &n);
    for (i = 0; i < n; i++)
        if (item_selectable(&it[i]) && mnemonic_of(it[i].text) == c) {
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
    default:               return 0;
    }
}

/* u_strlen is kept for the phases that follow (tips, combo text); reference it
 * so -Wunused-function does not fire on a file that is still growing. */
int uoc_text_len(const char *s);
int uoc_text_len(const char *s) { return u_strlen(s); }

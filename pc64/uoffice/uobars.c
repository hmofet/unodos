/* ===========================================================================
 * uobars.c - the Office 97 status bar, ruler and Assistant (phase 6d).
 * ======================================================================== */
#include "uobars.h"
#include "uoicons.h"

static int b_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void b_cpy(char *d, const char *s, int cap)
{ int i = 0; if (!d || cap <= 0) return;
  while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }

/* ---- the status bar -------------------------------------------------------- */
int uob_status_h(void) { return fb_text_h() + uoc_look_97()->pad + 2; }

/* The four mode cells are a fixed strip at the right, before the book, so a
 * hit test and a paint agree without either measuring text. */
static int cell_w(void) { return fb_text_w("OVR") + uoc_look_97()->pad * 2; }
static int cell_x(int x, int w, int i)
{
    const uoc_look *k = uoc_look_97();
    int book = k->icon_px + k->pad;
    return x + w - book - (4 - i) * cell_w() - k->pad;
}

void uob_status_render(const uob_status *st, int x, int y, int w)
{
    const uoc_look *k = uoc_look_97();
    int h = uob_status_h(), ty = y + (h - fb_text_h()) / 2, i;
    static const char *const kName[4] = { "REC", "TRK", "EXT", "OVR" };
    int on[4];
    if (!st) return;
    on[0] = st->rec; on[1] = st->trk; on[2] = st->ext; on[3] = st->ovr;

    fb_fill_rect(x, y, w, h, k->face);
    uoc_etch_h(k, x, y, w);

    if (st->page) fb_text(x + k->pad * 2, ty, st->page, k->text, -1);
    if (st->pos)  fb_text(x + w / 3, ty, st->pos, k->text, -1);

    for (i = 0; i < 4; i++) {
        int cx = cell_x(x, w, i);
        uoc_bevel(cx, y + 2, cell_w(), h - 4, k->shadow, k->hilight, 1);
        fb_text(cx + k->pad, ty, kName[i],
                on[i] ? k->text : k->gray_text, -1);
    }
    {   /* the spelling book: a tick when clean, a cross when not, and the
         * pages flicker while a background check is running */
        int bx = x + w - k->icon_px - k->pad, by = y + (h - k->icon_px) / 2;
        uoc_draw_icon(k, bx, by, UOI_SPELL, 0);
        if (st->spell_busy)
            fb_frame_rect(bx - 1, by - 1, k->icon_px + 2, k->icon_px + 2,
                          k->shadow);
        else if (st->spell_errors)
            fb_frame_rect(bx - 1, by - 1, k->icon_px + 2, k->icon_px + 2,
                          FB_RGB(0xC0,0x00,0x00));
    }
}

int uob_status_hit(int x, int y, int w, int mx, int my)
{
    const uoc_look *k = uoc_look_97();
    int h = uob_status_h(), i;
    if (my < y || my >= y + h) return UOB_CELL_NONE;
    for (i = 0; i < 4; i++) {
        int cx = cell_x(x, w, i);
        if (mx >= cx && mx < cx + cell_w()) return UOB_CELL_REC + i;
    }
    if (mx >= x + w - k->icon_px - k->pad) return UOB_CELL_SPELL;
    return UOB_CELL_NONE;
}

/* ---- the ruler ------------------------------------------------------------- */
#define MARK_W 7                     /* an indent marker's width             */

void uob_ruler_init(uob_ruler *r, int text_x, int text_w)
{
    int i;
    if (!r) return;
    for (i = 0; i < (int)sizeof *r; i++) ((char *)r)[i] = 0;
    r->text_x = text_x;
    r->text_w = text_w;
    r->right = text_w;
    r->pick = UOB_TAB_LEFT;
    r->drag = UOB_DRAG_NONE;
    r->drag_tab = -1;
}
int uob_ruler_h(void) { return fb_text_h() + uoc_look_97()->pad * 2 + 2; }

/* the tab-type selector button at the far left */
static int sel_w(void) { return uoc_look_97()->icon_px + 2; }

/* A tab stop's glyph: L, an upside-down T, a mirrored L, or one with a dot. */
static void tab_glyph(int x, int y, int type, fb_px c)
{
    fb_hline(x - 3, y + 4, 7, c);
    switch (type) {
    case UOB_TAB_CENTER:  fb_vline(x,     y, 5, c); break;
    case UOB_TAB_RIGHT:   fb_vline(x + 2, y, 5, c); break;
    case UOB_TAB_DECIMAL: fb_vline(x,     y, 5, c); fb_pixel(x + 2, y + 4, c);
                          break;
    default:              fb_vline(x - 3, y, 5, c); break;
    }
}
/* The three indent markers: a down triangle (first line), an up triangle
 * (hanging), and the square under the hanging one that drags both. */
static void tri(int x, int y, int down, fb_px c)
{
    int i;
    for (i = 0; i < 4; i++)
        fb_hline(x - (down ? i : 3 - i), y + i, (down ? i : 3 - i) * 2 + 1, c);
}

void uob_ruler_render(const uob_ruler *r, int x, int y, int w)
{
    const uoc_look *k = uoc_look_97();
    int h = uob_ruler_h(), tx = x + sel_w() + r->text_x, i;
    int ty = y + 2, bar_h = h - 10;
    if (!r) return;

    fb_fill_rect(x, y, w, h, k->face);

    /* the tab-type selector */
    fb_fill_rect(x + 1, y + 1, sel_w(), h - 2, k->face);
    uoc_raised(k, x + 1, y + 1, sel_w(), h - 2);
    tab_glyph(x + 1 + sel_w() / 2, y + h / 2 - 4, r->pick, k->text);

    /* the ruler face: the text column is white, the margins are grey */
    fb_fill_rect(x + sel_w() + 2, ty, w - sel_w() - 4, bar_h, k->shadow);
    fb_fill_rect(tx, ty, r->text_w, bar_h, k->hilight);
    uoc_bevel(x + sel_w() + 2, ty, w - sel_w() - 4, bar_h,
              k->shadow, k->hilight, 1);

    /* the inch/cm ticks, every 32 px with a half-height mark between */
    for (i = 0; i <= r->text_w; i += 16) {
        int px = tx + i;
        if (i % 32 == 0) fb_vline(px, ty + bar_h / 2 - 2, 4, k->text);
        else             fb_pixel(px, ty + bar_h / 2, k->text);
    }

    for (i = 0; i < r->ntab; i++)
        tab_glyph(tx + r->tab[i], ty + bar_h - 5, r->tabtype[i], k->text);

    /* the markers, drawn last so they sit over the ticks */
    tri(tx + r->first, ty, 1, k->text);
    tri(tx + r->hang,  ty + bar_h - 4, 0, k->text);
    fb_fill_rect(tx + r->hang - 3, y + h - 5, MARK_W, 4, k->text);
    tri(tx + r->right, ty + bar_h - 4, 0, k->text);
}

static int near_mark(int mx, int px) { return mx >= px - 5 && mx <= px + 5; }

int uob_ruler_handle(uob_ruler *r, const unoui_event *e, int x, int y, int w)
{
    int h = uob_ruler_h(), tx = x + sel_w() + r->text_x, i, rel;
    (void)w;
    if (!r || !e) return 0;

    if (e->kind == UI_EV_MOUSE_MOVE && r->drag != UOB_DRAG_NONE) {
        rel = e->x - tx;
        if (rel < 0) rel = 0;
        if (rel > r->text_w) rel = r->text_w;
        switch (r->drag) {
        case UOB_DRAG_FIRST: r->first = rel; break;
        case UOB_DRAG_HANG:  r->hang  = rel; break;
        case UOB_DRAG_BOTH: {
            int d = rel - r->hang;
            r->hang += d; r->first += d;
            break;
        }
        case UOB_DRAG_RIGHT: r->right = rel; break;
        case UOB_DRAG_TAB:
            if (r->drag_tab >= 0 && r->drag_tab < r->ntab)
                r->tab[r->drag_tab] = rel;
            break;
        default: break;
        }
        return 1;
    }
    if (e->kind == UI_EV_MOUSE_UP && r->drag != UOB_DRAG_NONE) {
        r->drag = UOB_DRAG_NONE;
        r->drag_tab = -1;
        return 1;
    }
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    if (e->y < y || e->y >= y + h) return 0;

    /* the selector cycles the tab type, exactly as Word's did */
    if (e->x < x + sel_w() + 2) {
        r->pick = (r->pick + 1) % 4;
        return 1;
    }

    /* markers first: they overlap the ruler face and win the hit */
    if (e->y < y + 2 + 6 && near_mark(e->x, tx + r->first))
        { r->drag = UOB_DRAG_FIRST; return 1; }
    if (e->y >= y + h - 6 && near_mark(e->x, tx + r->hang))
        { r->drag = UOB_DRAG_BOTH; return 1; }
    if (near_mark(e->x, tx + r->hang) && e->y >= y + h - 12)
        { r->drag = UOB_DRAG_HANG; return 1; }
    if (near_mark(e->x, tx + r->right))
        { r->drag = UOB_DRAG_RIGHT; return 1; }

    for (i = 0; i < r->ntab; i++)
        if (near_mark(e->x, tx + r->tab[i]))
            { r->drag = UOB_DRAG_TAB; r->drag_tab = i; return 1; }

    /* a click on the bare ruler sets a new tab stop of the selected type */
    if (r->ntab < UOB_MAXTAB) {
        rel = e->x - tx;
        if (rel >= 0 && rel <= r->text_w) {
            r->tab[r->ntab] = rel;
            r->tabtype[r->ntab] = r->pick;
            r->ntab++;
            return 1;
        }
    }
    return 1;
}

/* ---- the Assistant --------------------------------------------------------- */
#define AS_W  40
#define AS_H  40

static int as_balloon_w(const uob_assist *a)
{
    const uoc_look *k = uoc_look_97();
    int w = fb_text_w("What would you like to do?") + k->pad * 4, i;
    for (i = 0; i < a->ntopic; i++) {
        int tw = fb_text_w(a->topic[i]) + k->pad * 6;
        if (tw > w) w = tw;
    }
    return w;
}
static int as_balloon_h(const uob_assist *a)
{
    const uoc_look *k = uoc_look_97();
    return k->pad * 3 + fb_text_h() * 2 + k->pad * 2 +
           a->ntopic * (fb_text_h() + k->pad) + k->pad * 2 + fb_text_h() + 4;
}
static void as_balloon_rect(const uob_assist *a, int *x, int *y, int *w, int *h)
{
    *w = as_balloon_w(a);
    *h = as_balloon_h(a);
    *x = a->x + AS_W / 2 - *w / 2;
    *y = a->y - *h - 6;
    if (*x < 2) *x = 2;
    if (*x + *w > FB_W - 2) *x = FB_W - 2 - *w;
    if (*y < 2) *y = a->y + AS_H + 6;
}

void uob_assist_open(uob_assist *a, int x, int y)
{
    int i;
    if (!a) return;
    for (i = 0; i < (int)sizeof *a; i++) ((char *)a)[i] = 0;
    a->x = x; a->y = y;
    a->open = 1;
    a->hot = -1;
    b_cpy(a->query, "", UOB_MAXQUERY);
}
void uob_assist_close(uob_assist *a) { if (a) { a->open = 0; a->balloon = 0; } }
void uob_assist_ask(uob_assist *a, const char *const *topics, int n)
{
    if (!a) return;
    a->topic = topics; a->ntopic = n;
    a->balloon = 1;
    a->bulb = 0;
    a->hot = -1;
}
int uob_assist_taken(uob_assist *a)
{
    int t;
    if (!a) return -1;
    t = a->hot;
    return t;
}

static int topic_at(const uob_assist *a, int mx, int my)
{
    const uoc_look *k = uoc_look_97();
    int x, y, w, h, i, ty;
    as_balloon_rect(a, &x, &y, &w, &h);
    ty = y + k->pad * 2 + fb_text_h() + k->pad * 2 + fb_text_h() + k->pad * 2;
    for (i = 0; i < a->ntopic; i++) {
        int ry = ty + i * (fb_text_h() + k->pad);
        if (mx >= x + k->pad && mx < x + w - k->pad &&
            my >= ry && my < ry + fb_text_h() + k->pad) return i;
    }
    return -1;
}

int uob_assist_handle(uob_assist *a, const unoui_event *e)
{
    const uoc_look *k = uoc_look_97();
    int x, y, w, h;
    if (!a || !a->open || !e) return 0;

    if (e->kind == UI_EV_TICK) { a->tick++; return 0; }

    if (e->kind == UI_EV_MOUSE_MOVE) {
        if (a->drag) { a->x = e->x - a->drag_dx; a->y = e->y - a->drag_dy; return 1; }
        if (a->balloon) { a->hot = topic_at(a, e->x, e->y); return a->hot >= 0; }
        return 0;
    }
    if (e->kind == UI_EV_MOUSE_UP) {
        if (a->drag) { a->drag = 0; return 1; }
        return 0;
    }
    if (e->kind == UI_EV_MOUSE_DOWN) {
        if (a->balloon) {
            as_balloon_rect(a, &x, &y, &w, &h);
            /* the Close button along the balloon's bottom edge */
            if (e->x >= x && e->x < x + w && e->y >= y && e->y < y + h) {
                if (e->y >= y + h - fb_text_h() - k->pad * 2) {
                    a->balloon = 0;
                    return 1;
                }
                a->hot = topic_at(a, e->x, e->y);
                return 1;
            }
        }
        /* the character itself: click to ask, drag to move */
        if (e->x >= a->x && e->x < a->x + AS_W &&
            e->y >= a->y && e->y < a->y + AS_H) {
            a->drag = 1;
            a->drag_dx = e->x - a->x; a->drag_dy = e->y - a->y;
            if (!a->balloon) { a->balloon = 1; a->bulb = 0; }
            return 1;
        }
        return 0;
    }
    if (e->kind == UI_EV_CHAR && a->balloon && e->ch >= ' ') {
        int n = b_strlen(a->query);
        if (n < UOB_MAXQUERY - 1) { a->query[n] = (char)e->ch; a->query[n+1] = 0; }
        return 1;
    }
    return 0;
}

void uob_assist_render(const uob_assist *a)
{
    const uoc_look *k = uoc_look_97();
    int x, y, w, h, i, ty;
    if (!a || !a->open) return;

    if (a->balloon) {
        as_balloon_rect(a, &x, &y, &w, &h);
        fb_fill_rect(x, y, w, h, k->tip_bg);
        fb_frame_rect(x, y, w, h, k->dkshadow);
        /* the tail, pointing at the character */
        for (i = 0; i < 6; i++)
            fb_hline(a->x + AS_W / 2 - i, y + h + i, i * 2 + 1,
                     (i == 5) ? k->dkshadow : k->tip_bg);

        ty = y + k->pad * 2;
        fb_text(x + k->pad * 2, ty, "What would you like to do?", k->text, -1);
        ty += fb_text_h() + k->pad;
        /* the query box */
        fb_fill_rect(x + k->pad * 2, ty, w - k->pad * 4, fb_text_h() + 4,
                     k->hilight);
        uoc_sunken(k, x + k->pad * 2, ty, w - k->pad * 4, fb_text_h() + 4);
        fb_text(x + k->pad * 2 + 3, ty + 2, a->query, k->text, -1);
        ty += fb_text_h() + 4 + k->pad;

        for (i = 0; i < a->ntopic; i++) {
            int ry = ty + i * (fb_text_h() + k->pad);
            fb_px c = k->text;
            if (i == a->hot) {
                fb_fill_rect(x + k->pad, ry, w - k->pad * 2,
                             fb_text_h() + k->pad, k->sel);
                c = k->sel_text;
            }
            /* the numbered blue bullet Office answered with */
            fb_fill_rect(x + k->pad * 2, ry + fb_text_h() / 2 - 1, 4, 4,
                         (i == a->hot) ? k->sel_text : k->sel);
            fb_text(x + k->pad * 2 + 8, ry + k->pad / 2, a->topic[i], c, -1);
        }
        {   /* the Close button along the bottom */
            int bw = fb_text_w("Close") + k->pad * 3;
            int bx = x + w - bw - k->pad, by = y + h - fb_text_h() - k->pad * 2 + 1;
            fb_fill_rect(bx, by, bw, fb_text_h() + k->pad, k->face);
            uoc_raised(k, bx, by, bw, fb_text_h() + k->pad);
            fb_text(bx + k->pad + 2, by + k->pad / 2, "Close", k->text, -1);
        }
    }

    /* "Uno" itself: our own character on its own little raised card, which is
     * what stands in for Office's frameless always-on-top character window. */
    {
        int s = k->icon_px;
        fb_fill_rect(a->x, a->y, AS_W, AS_H, k->face);
        uoc_raised(k, a->x, a->y, AS_W, AS_H);
        uoc_draw_icon(k, a->x + (AS_W - s) / 2, a->y + (AS_H - s) / 2,
                      UOI_ASSISTANT, 0);
        /* the idle animation: it blinks, on a slow beat off the tick stream */
        if ((a->tick / 24) % 8 == 0)
            fb_fill_rect(a->x + (AS_W - s) / 2 + 4,
                         a->y + (AS_H - s) / 2 + 5, s - 8, 3, k->face);
        /* the lightbulb that means "I have a tip" */
        if (a->bulb) {
            int bx = a->x + AS_W - 8, by = a->y - 2;
            fb_fill_rect(bx, by, 6, 6, FB_RGB(0xFF,0xFF,0x00));
            fb_frame_rect(bx, by, 6, 6, k->dkshadow);
            fb_hline(bx + 1, by + 7, 4, k->shadow);
        }
    }
}

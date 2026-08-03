/* ===========================================================================
 * uos_render.c - one slide renderer, for every destination.
 * (OFFICE97-PLAN §7 phase 11; SPEC S-UOS-03's B&W view lives here too.)
 *
 * The editing view, a sorter thumbnail, the notes page, the handout grid and
 * a full-screen show are the SAME function with a different rectangle.  That
 * is not a tidiness preference: a slide that is only correct at 100% is a
 * slide the sorter lies about, and the show is where you find out.
 *
 * Everything is drawn from fb.h's smallest primitives - fb_fill_rect,
 * fb_hline, fb_pixel, fb_text - and nothing else.  fb_round_rect and
 * fb_grad_v exist and would have been convenient, but each one a module
 * imports is another name pc64_modload.c has to export, and a polygon filler
 * has to exist here regardless: fb has no polygon.  So the scanline filler
 * below serves the polygons, the ellipse, the gradients and the patterns, and
 * UOSHOW.UNO imports the same short list UOWORD.UNO does.
 *
 * SPANS CARRY THE FILL.  fill_shape() emits one horizontal span at a time and
 * hands each to a span painter, so solid, gradient and pattern are three span
 * painters over one geometry walk rather than three copies of the walk.
 * ======================================================================== */
#include "uoshow.h"

/* from uos_model.c - the parts the renderer needs without seeing uos_slide */
int  uos_slide_has_bg(const uos_pres *p, int i, uos_fill *out);
int  uos_slide_omits_master(const uos_pres *p, int i);
int  uos_ph_is_text(int ph);

/* ---- the metrics seam --------------------------------------------------------
 * Same shape as uoword's, same reason: the host gate draws with the 8x8
 * bitmap font and pc64 draws with the kerned TTF engine at a size that
 * depends on the zoom, and this file must not be able to tell. */
static uos_metrics g_m;
static int g_have_m;

void uos_set_metrics(const uos_metrics *m)
{ if (m) { g_m = *m; g_have_m = 1; } else g_have_m = 0; }

/* px = the pixel size this run should draw at, already scaled. */
static int mtx_w(const char *s, int n, const uos_chp *c, int px)
{
    if (g_have_m && g_m.text_w) return g_m.text_w(s, n, c, px, g_m.ctx);
    (void)c; (void)px;
    { int w = 0, i; char b[2]; b[1] = 0;
      for (i = 0; i < n; i++) { b[0] = s[i]; w += fb_text_w(b); }
      return w; }
}
static int mtx_h(const uos_chp *c, int px)
{
    if (g_have_m && g_m.height) return g_m.height(c, px, g_m.ctx);
    (void)c; (void)px;
    return fb_text_h();
}
static int g_cx0, g_cy0, g_cx1, g_cy1;   /* forward: see the clip rect below */
static void mtx_draw(int x, int y, const char *s, int n, const uos_chp *c,
                     int px, fb_px col)
{
    /* fb_text clips to the SCREEN, never to our rectangle, so a run that
     * starts outside the slide is dropped whole rather than half-drawn. */
    if (y < g_cy0 - 2 || y > g_cy1 || x > g_cx1) return;
    if (g_have_m && g_m.draw) { g_m.draw(x, y, s, n, c, px, col, g_m.ctx); return; }
    (void)c; (void)px;
    { char b[2]; int i; b[1] = 0;
      for (i = 0; i < n; i++) { b[0] = s[i]; fb_text(x, y, b, col, -1); x += fb_text_w(b); } }
}

/* ---- the clip rect ------------------------------------------------------------
 * A shape may extend past the slide edge - PowerPoint lets you drag one half
 * off, and a deck full of them is normal.  On a full-screen show that is
 * harmless; in the SORTER it is not, because the overhang lands on the
 * neighbouring thumbnail.  So the renderer clips to the slide rectangle
 * itself.
 *
 * It cannot use fb_set_clip: fb.c's primitives clip to the SCREEN, and the
 * settable clip window governs fb_aa.c's alpha primitives only (the trap
 * uochrome.h records - it is why combo text is truncated rather than
 * clipped).  Every span and every line therefore passes through here. */
static void clip_set(int x, int y, int w, int h)
{ g_cx0 = x; g_cy0 = y; g_cx1 = x + w - 1; g_cy1 = y + h - 1; }

/* An override the SHOW uses: a transition reveals the incoming slide through
 * a moving window, and "render this slide but only inside that window" is the
 * whole mechanism.  One render call per band beats a second framebuffer,
 * which this OS does not hand a module. */
static int g_ovr, g_ox0, g_oy0, g_ow, g_oh;
void uos_clip(int x, int y, int w, int h)
{ g_ovr = 1; g_ox0 = x; g_oy0 = y; g_ow = w; g_oh = h; }
void uos_clip_off(void) { g_ovr = 0; }
static int clip_row(int y) { return y >= g_cy0 && y <= g_cy1; }

/* ---- colour ------------------------------------------------------------------ */
fb_px uos_bw(fb_px c)
{
    /* PowerPoint's Black and White view is not a greyscale photo of the
     * slide: it pushes everything to black, white or one mid grey so the
     * result reads on a fax and on a mono overhead.  Luma thresholds do that
     * in three bands. */
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    int y = (r * 77 + g * 151 + b * 28) >> 8;
    if (y < 64)  return FB_RGB(0, 0, 0);
    if (y > 192) return FB_RGB(255, 255, 255);
    return FB_RGB(128, 128, 128);
}

typedef struct {
    uos_pres *p;
    int bw;
} uos_ctx;

static fb_px col_of(const uos_ctx *cx, fb_px c)
{
    fb_px v = uos_color(cx->p, c);
    return cx->bw ? uos_bw(v) : v;
}

/* ---- span painters ------------------------------------------------------------
 * Each takes one horizontal run of pixels and the shape's bounding box, so a
 * gradient knows where it is in the shape and a pattern knows its phase. */
typedef struct {
    int kind, pattern;
    fb_px c1, c2;
    int bx, by, bw, bh;           /* the shape's pixel bounding box           */
} uos_span;

static fb_px lerp(fb_px a, fb_px b, int num, int den)
{
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    if (den <= 0) den = 1;
    return FB_RGB(ar + (br - ar) * num / den,
                  ag + (bg - ag) * num / den,
                  ab + (bb - ab) * num / den);
}

static int pat_on(int pat, int x, int y)
{
    switch (pat) {
    case UOS_P_H:     return (y & 3) == 0;
    case UOS_P_V:     return (x & 3) == 0;
    case UOS_P_FDIAG: return ((x + y) & 3) == 0;
    case UOS_P_BDIAG: return ((x - y) & 3) == 0;
    case UOS_P_GRID:  return (x & 3) == 0 || (y & 3) == 0;
    case UOS_P_DOTS:  return (x & 3) == 0 && (y & 3) == 0;
    default:          return 1;
    }
}

static void paint_span(const uos_span *s, int x0, int x1, int y)
{
    int x;
    if (!clip_row(y)) return;
    if (x0 < g_cx0) x0 = g_cx0;
    if (x1 > g_cx1) x1 = g_cx1;
    if (x1 < x0) return;
    switch (s->kind) {
    case UOS_F_SOLID:
        fb_hline(x0, y, x1 - x0 + 1, s->c1);
        return;
    case UOS_F_GRAD_V:
        fb_hline(x0, y, x1 - x0 + 1, lerp(s->c1, s->c2, y - s->by, s->bh - 1));
        return;
    case UOS_F_GRAD_H:
        for (x = x0; x <= x1; x++)
            fb_pixel(x, y, lerp(s->c1, s->c2, x - s->bx, s->bw - 1));
        return;
    case UOS_F_PATTERN:
        fb_hline(x0, y, x1 - x0 + 1, s->c2);
        for (x = x0; x <= x1; x++)
            if (pat_on(s->pattern, x, y)) fb_pixel(x, y, s->c1);
        return;
    default:
        return;
    }
}

/* ---- the scanline polygon filler ---------------------------------------------
 * Even-odd rule, integer edge walking, spans handed to paint_span.  This is
 * the one piece of real geometry in the file; everything else is a table. */
#define UOS_MAXPT 16

static void fill_poly(const short *xy, int n, const uos_span *sp)
{
    int ymin = 0, ymax = 0, i, y;
    if (n < 3) return;
    ymin = ymax = xy[1];
    for (i = 1; i < n; i++) {
        if (xy[i * 2 + 1] < ymin) ymin = xy[i * 2 + 1];
        if (xy[i * 2 + 1] > ymax) ymax = xy[i * 2 + 1];
    }
    for (y = ymin; y <= ymax; y++) {
        int xs[UOS_MAXPT * 2], nx = 0, a, b;
        for (i = 0; i < n; i++) {
            int j = (i + 1) % n;
            int y0 = xy[i * 2 + 1], y1 = xy[j * 2 + 1];
            int x0 = xy[i * 2],     x1 = xy[j * 2];
            if (y0 == y1) continue;
            if ((y >= y0 && y < y1) || (y >= y1 && y < y0)) {
                if (nx < UOS_MAXPT * 2)
                    xs[nx++] = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
            }
        }
        for (a = 1; a < nx; a++) {          /* insertion sort: nx is tiny     */
            int v = xs[a];
            b = a - 1;
            while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = v;
        }
        for (a = 0; a + 1 < nx; a += 2) paint_span(sp, xs[a], xs[a + 1], y);
    }
}

static void fill_ellipse(int x, int y, int w, int h, const uos_span *sp)
{
    int cx2 = w, cy2 = h, i;
    if (w <= 0 || h <= 0) return;
    for (i = 0; i < h; i++) {
        /* (2i-h)^2/h^2 + (2d/w)^2 = 1  ->  half-width d at row i */
        long dy = (long)(2 * i - cy2 + 1);
        long t  = (long)cy2 * cy2 - dy * dy;
        long d;
        if (t < 0) continue;
        d = 0;
        { long lo = 0, hi = cx2; /* integer sqrt of t*(w*w)/(h*h) scaled */
          long want = t * cx2 * cx2 / ((long)cy2 * cy2);
          while (lo <= hi) { long mid = (lo + hi) / 2;
              if (mid * mid <= want) { d = mid; lo = mid + 1; } else hi = mid - 1; } }
        paint_span(sp, x + (cx2 - (int)d) / 2, x + (cx2 + (int)d) / 2 - 1, y + i);
    }
}

static void fill_roundrect(int x, int y, int w, int h, int rad, const uos_span *sp)
{
    int i;
    if (w <= 0 || h <= 0) return;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    for (i = 0; i < h; i++) {
        int inset = 0;
        int dy = (i < rad) ? rad - i : (i >= h - rad ? i - (h - rad) + 1 : 0);
        if (dy > 0) {
            long lo = 0, hi = rad, d = 0, want = (long)rad * rad - (long)dy * dy;
            if (want < 0) want = 0;
            while (lo <= hi) { long mid = (lo + hi) / 2;
                if (mid * mid <= want) { d = mid; lo = mid + 1; } else hi = mid - 1; }
            inset = rad - (int)d;
        }
        paint_span(sp, x + inset, x + w - 1 - inset, y + i);
    }
}

/* ---- outlines ------------------------------------------------------------------ */
static void line_px(int x0, int y0, int x1, int y1, fb_px c, int width, int kind)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2, e2, step = 0;
    if (width < 1) width = 1;
    for (;;) {
        int on = 1;
        if (kind == UOS_L_DASH) on = ((step / 4) & 1) == 0;
        else if (kind == UOS_L_DOT) on = (step & 1) == 0;
        if (on && x0 >= g_cx0 && x0 <= g_cx1 && clip_row(y0)) {
            if (width == 1) fb_pixel(x0, y0, c);
            else {
                int rx = x0 - width / 2, ry = y0 - width / 2, rw = width, rh = width;
                if (rx < g_cx0) { rw -= g_cx0 - rx; rx = g_cx0; }
                if (ry < g_cy0) { rh -= g_cy0 - ry; ry = g_cy0; }
                if (rx + rw - 1 > g_cx1) rw = g_cx1 - rx + 1;
                if (ry + rh - 1 > g_cy1) rh = g_cy1 - ry + 1;
                if (rw > 0 && rh > 0) fb_fill_rect(rx, ry, rw, rh, c);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
        step++;
    }
}
static void stroke_poly(const short *xy, int n, fb_px c, int width, int kind)
{
    int i;
    for (i = 0; i < n; i++) {
        int j = (i + 1) % n;
        line_px(xy[i * 2], xy[i * 2 + 1], xy[j * 2], xy[j * 2 + 1], c, width, kind);
    }
}
static void stroke_ellipse(int x, int y, int w, int h, fb_px c, int width, int kind)
{
    int i, prevx = 0, prevy = 0;
    /* 32 segments is invisible at slide scale and needs no trig table: the
     * points come off the same integer half-width the filler uses. */
    for (i = 0; i <= 32; i++) {
        int step = i % 32, quad, row, hw, px, py;
        (void)quad;
        row = (h - 1) * (step < 16 ? step : 32 - step) / 16;
        { long dy = (long)(2 * row - h + 1);
          long t = (long)h * h - dy * dy, lo = 0, hi = w, d = 0, want;
          if (t < 0) t = 0;
          want = t * w * w / ((long)h * h);
          while (lo <= hi) { long mid = (lo + hi) / 2;
              if (mid * mid <= want) { d = mid; lo = mid + 1; } else hi = mid - 1; }
          hw = (int)d / 2; }
        px = x + w / 2 + (step < 16 ? hw : -hw);
        py = y + row;
        if (i) line_px(prevx, prevy, px, py, c, width, kind);
        prevx = px; prevy = py;
    }
}

/* ---- text inside a shape -------------------------------------------------------
 * Word-wrapped, one paragraph at a time, indented by outline level, bulleted
 * where the paragraph says so, aligned three ways.  Vertically CENTRED in the
 * frame, which is what a PowerPoint placeholder does and the single most
 * visible difference from a word processor's text box. */
#define IND_PER_LEVEL 28          /* slide points */

static int wrap_one(const char *s, int n, const uos_chp *c, int px, int avail,
                    int *used)
{
    int w = 0, i = 0, last_space = -1;
    while (i < n) {
        int cw = mtx_w(s + i, 1, c, px);
        if (w + cw > avail && i > 0) break;
        if (s[i] == ' ') last_space = i;
        w += cw; i++;
    }
    if (i < n && last_space > 0) i = last_space + 1;
    *used = i;
    return w;
}

static void draw_body(const uos_ctx *cx, const uos_shape *sh,
                      int slide, int z, int bx, int by, int bw, int bh,
                      int num, int den)
{
    int i, np = uos_text_paras(cx->p, slide, z);
    int total = 0, y;
    (void)sh;
    struct { int at, n, x, w, px, h, pi; const uos_para *pa; } line[64];
    int nline = 0;

    for (i = 0; i < np && nline < 64; i++) {
        const uos_para *pa = uos_para_at(cx->p, slide, z, i);
        int len = 0;
        const char *t = uos_para_text(cx->p, slide, z, i, &len);
        int px = (int)pa->chp.size * num / den;
        int ind = pa->level * IND_PER_LEVEL * num / den;
        int bul = pa->bullet ? mtx_w("\x95", 1, &pa->chp, px) + px / 3 : 0;
        int avail = bw - ind - bul;
        int off = 0;
        if (px < 4) px = 4;
        if (avail < px) avail = px;
        if (!len) {                                  /* an empty paragraph    */
            line[nline].at = 0; line[nline].n = 0; line[nline].pa = pa;
            line[nline].pi = i;
            line[nline].x = ind + bul; line[nline].w = 0;
            line[nline].px = px; line[nline].h = mtx_h(&pa->chp, px);
            total += line[nline].h; nline++;
            continue;
        }
        while (off < len && nline < 64) {
            int used = 0;
            int w = wrap_one(t + off, len - off, &pa->chp, px, avail, &used);
            if (used <= 0) used = 1;
            line[nline].at = off; line[nline].n = used; line[nline].pa = pa;
            line[nline].pi = i;
            line[nline].x = ind + bul; line[nline].w = w;
            line[nline].px = px; line[nline].h = mtx_h(&pa->chp, px);
            total += line[nline].h;
            nline++;
            off += used;
        }
    }

    y = by + (bh - total) / 2;
    if (y < by) y = by;
    for (i = 0; i < nline; i++) {
        const uos_para *pa = line[i].pa;
        int len = 0;
        const char *t = uos_para_text(cx->p, slide, z, line[i].pi, &len);
        int x = bx + line[i].x;
        fb_px col = col_of(cx, pa->chp.color);

        if (pa->align == UOS_AL_CENTER) x = bx + (bw - line[i].w) / 2;
        else if (pa->align == UOS_AL_RIGHT) x = bx + bw - line[i].w;

        if (pa->bullet && line[i].at == 0 && line[i].n > 0) {
            char b[2]; b[0] = (char)pa->bullet; b[1] = 0;
            mtx_draw(x - mtx_w(b, 1, &pa->chp, line[i].px) - line[i].px / 3,
                     y, b, 1, &pa->chp, line[i].px, col);
        }
        if (pa->chp.shadow) {
            int d = line[i].px / 12 + 1;
            mtx_draw(x + d, y + d, t + line[i].at, line[i].n, &pa->chp,
                     line[i].px, col_of(cx, UOS_SCHEME_COLOR(UOS_C_SHADOW)));
        }
        mtx_draw(x, y, t + line[i].at, line[i].n, &pa->chp, line[i].px, col);
        if (pa->chp.underline)
            fb_hline(x, y + line[i].h - 1, line[i].w, col);
        y += line[i].h;
    }
}

/* ---- one shape ------------------------------------------------------------------ */
static void map_path(const uos_map *m, const uos_shape *sh,
                     const short *src, int n, short *dst)
{
    int i;
    for (i = 0; i < n; i++) {
        int px = sh->x + (int)src[i * 2]     * sh->w / UOS_GEOM_BOX;
        int py = sh->y + (int)src[i * 2 + 1] * sh->h / UOS_GEOM_BOX;
        dst[i * 2]     = (short)(m->x + px * m->num / m->den);
        dst[i * 2 + 1] = (short)(m->y + py * m->num / m->den);
    }
}

static void draw_shape(const uos_ctx *cx, const uos_map *m, int slide, int z,
                       const uos_shape *sh, int flags)
{
    short src[UOS_MAXPT * 2], dst[UOS_MAXPT * 2];
    int n, kind = uos_geom_kind(sh->geom);
    int bx = m->x + sh->x * m->num / m->den;
    int by = m->y + sh->y * m->num / m->den;
    int bw = sh->w * m->num / m->den;
    int bh = sh->h * m->num / m->den;
    uos_span sp;

    if (sh->hidden) return;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;

    n = uos_geom_path(sh->geom, sh->adj, src, UOS_MAXPT);
    map_path(m, sh, src, n, dst);

    sp.kind = sh->fill.kind;
    sp.pattern = sh->fill.pattern;
    sp.c1 = col_of(cx, sh->fill.c1);
    sp.c2 = col_of(cx, sh->fill.c2);
    sp.bx = bx; sp.by = by; sp.bw = bw; sp.bh = bh;

    /* the shadow is the same geometry, offset, in one flat colour */
    if (sh->shadow.on && sh->fill.kind != UOS_F_NONE) {
        uos_span sh_sp = sp;
        int dx = sh->shadow.dx * m->num / m->den;
        int dy = sh->shadow.dy * m->num / m->den;
        int i;
        sh_sp.kind = UOS_F_SOLID;
        sh_sp.c1 = col_of(cx, sh->shadow.c ? sh->shadow.c
                                           : UOS_SCHEME_COLOR(UOS_C_SHADOW));
        if (kind == UOS_GK_ELLIPSE) fill_ellipse(bx + dx, by + dy, bw, bh, &sh_sp);
        else if (kind == UOS_GK_ROUNDRECT)
            fill_roundrect(bx + dx, by + dy, bw, bh, (bw < bh ? bw : bh) / 5, &sh_sp);
        else if (kind == UOS_GK_POLY) {
            short off[UOS_MAXPT * 2];
            for (i = 0; i < n * 2; i += 2)
            { off[i] = (short)(dst[i] + dx); off[i + 1] = (short)(dst[i + 1] + dy); }
            fill_poly(off, n, &sh_sp);
        }
    }

    if (sh->fill.kind != UOS_F_NONE && kind != UOS_GK_LINE) {
        if (kind == UOS_GK_ELLIPSE)        fill_ellipse(bx, by, bw, bh, &sp);
        else if (kind == UOS_GK_ROUNDRECT) fill_roundrect(bx, by, bw, bh,
                                                (bw < bh ? bw : bh) / 5, &sp);
        else                               fill_poly(dst, n, &sp);
    }

    if (sh->line.kind != UOS_L_NONE) {
        fb_px lc = col_of(cx, sh->line.c);
        int lw = sh->line.width * m->num / m->den;
        if (lw < 1) lw = 1;
        if (kind == UOS_GK_ELLIPSE)      stroke_ellipse(bx, by, bw, bh, lc, lw, sh->line.kind);
        else if (kind == UOS_GK_LINE)    line_px(bx, by, bx + bw - 1, by + bh - 1,
                                                 lc, lw, sh->line.kind);
        else                             stroke_poly(dst, n, lc, lw, sh->line.kind);
    }

    if (!(flags & UOS_R_NOTEXT) && sh->para_n > 0) {
        int pad = 6 * m->num / m->den;
        draw_body(cx, sh, slide, z, bx + pad, by + pad,
                  bw - 2 * pad, bh - 2 * pad, m->num, m->den);
    }

    /* An EMPTY placeholder shows its dashed frame and prompt, and only in the
     * editor - never in a show, never on a thumbnail that stands for one. */
    if ((flags & UOS_R_PHFRAMES) && sh->ph != UOS_PH_NONE && sh->para_n == 0) {
        fb_px g = cx->bw ? FB_RGB(128,128,128) : FB_RGB(128, 128, 128);
        short f[8];
        const char *prompt = uos_ph_is_text(sh->ph)
            ? (sh->ph == UOS_PH_TITLE || sh->ph == UOS_PH_CTRTITLE
               ? "Click to add title" : "Click to add text")
            : "Double click to add an object";
        f[0]=(short)bx; f[1]=(short)by; f[2]=(short)(bx+bw-1); f[3]=(short)by;
        f[4]=(short)(bx+bw-1); f[5]=(short)(by+bh-1); f[6]=(short)bx; f[7]=(short)(by+bh-1);
        stroke_poly(f, 4, g, 1, UOS_L_DASH);
        if (bh > fb_text_h() + 4)
            fb_text(bx + (bw - fb_text_w(prompt)) / 2,
                    by + (bh - fb_text_h()) / 2, prompt, g, -1);
    }
}

/* ---- the slide ------------------------------------------------------------------ */
void uos_fit(int w, int h, int *ox, int *oy, int *ow, int *oh)
{
    int fw = w, fh = w * UOS_SLIDE_H / UOS_SLIDE_W;
    if (fh > h) { fh = h; fw = h * UOS_SLIDE_W / UOS_SLIDE_H; }
    if (ow) *ow = fw;
    if (oh) *oh = fh;
    if (ox) *ox = (w - fw) / 2;
    if (oy) *oy = (h - fh) / 2;
}

void uos_render(uos_pres *p, int slide, int x, int y, int w, int h,
                int flags, uos_map *map)
{
    uos_map m;
    uos_ctx cx;
    uos_fill bg;
    uos_span sp;
    int i, n, ox, oy, ow, oh;
    int master = uos_master(p, uos_slide_layout(p, slide) == UOS_AL_TITLE
                               ? UOS_M_TITLE : UOS_M_SLIDE);
    if (!p) return;

    uos_fit(w, h, &ox, &oy, &ow, &oh);
    m.x = x + ox; m.y = y + oy; m.w = ow; m.h = oh;
    m.num = ow; m.den = UOS_SLIDE_W;
    if (map) *map = m;

    cx.p = p;
    cx.bw = (flags & UOS_R_BW) ? 1 : 0;
    if (g_ovr) {
        int x0 = m.x > g_ox0 ? m.x : g_ox0;
        int y0 = m.y > g_oy0 ? m.y : g_oy0;
        int x1 = (m.x + m.w) < (g_ox0 + g_ow) ? m.x + m.w : g_ox0 + g_ow;
        int y1 = (m.y + m.h) < (g_oy0 + g_oh) ? m.y + m.h : g_oy0 + g_oh;
        clip_set(x0, y0, x1 - x0, y1 - y0);
        if (x1 <= x0 || y1 <= y0) return;
    } else {
        clip_set(m.x, m.y, m.w, m.h);
    }

    if (!(flags & UOS_R_NOBG)) {
        if (!uos_slide_has_bg(p, slide, &bg)) {
            bg.kind = UOS_F_SOLID;
            bg.c1 = UOS_SCHEME_COLOR(UOS_C_BG);
            bg.c2 = UOS_SCHEME_COLOR(UOS_C_BG);
            bg.pattern = 0;
        }
        sp.kind = bg.kind == UOS_F_NONE ? UOS_F_SOLID : bg.kind;
        sp.pattern = bg.pattern;
        sp.c1 = col_of(&cx, bg.c1);
        sp.c2 = col_of(&cx, bg.c2);
        sp.bx = m.x; sp.by = m.y; sp.bw = m.w; sp.bh = m.h;
        for (i = 0; i < m.h; i++) paint_span(&sp, m.x, m.x + m.w - 1, m.y + i);
    }

    if (!(flags & UOS_R_NOMASTER) && master >= 0 && !uos_slide_omits_master(p, slide)) {
        n = uos_shapes(p, master);
        for (i = 0; i < n; i++) {
            const uos_shape *sh = uos_shape_at_c(p, master, i);
            /* the master's placeholders are prompts for the slide's own text,
             * not content: draw its decoration, never its empty holders */
            if (sh->ph != UOS_PH_NONE) continue;
            draw_shape(&cx, &m, master, i, sh, flags & ~UOS_R_PHFRAMES);
        }
    }

    n = uos_shapes(p, slide);
    for (i = 0; i < n; i++)
        draw_shape(&cx, &m, slide, i, uos_shape_at_c(p, slide, i), flags);
}

void uos_to_slide(const uos_map *m, int sx, int sy, int *ox, int *oy)
{
    if (!m || m->num <= 0) return;
    if (ox) *ox = (sx - m->x) * m->den / m->num;
    if (oy) *oy = (sy - m->y) * m->den / m->num;
}
void uos_to_screen(const uos_map *m, int px, int py, int *ox, int *oy)
{
    if (!m) return;
    if (ox) *ox = m->x + px * m->num / m->den;
    if (oy) *oy = m->y + py * m->num / m->den;
}

int uos_hit(const uos_pres *p, int slide, int px, int py)
{
    int i, n = uos_shapes(p, slide);
    for (i = n - 1; i >= 0; i--) {          /* topmost first */
        const uos_shape *sh = uos_shape_at_c(p, slide, i);
        if (!sh || sh->hidden) continue;
        if (px >= sh->x && px < sh->x + sh->w &&
            py >= sh->y && py < sh->y + sh->h) return i;
    }
    return -1;
}

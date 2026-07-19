/* UnoDOS/PS2 software framebuffer primitives (see fb.h). Pure C, no platform
 * dependencies - shared verbatim by the host shim and the EE target. */
#include "fb.h"
#include "build/font_data.h"

fb_px fb[FB_BUF_PIX];

/* Optional clip window (absolute fb coords). All drawing is confined to the
 * intersection of this window and the screen. Default = whole screen, so any
 * caller that never touches it (the legacy core, every non-unoui port) is
 * unaffected. unoui sets it per-window so a widget can never overflow its
 * frame onto the desktop. */
#define CL_BIG (1 << 28)
static int cl_x0 = 0, cl_y0 = 0, cl_x1 = CL_BIG, cl_y1 = CL_BIG;

void fb_set_clip(int x, int y, int w, int h)
{
    cl_x0 = x; cl_y0 = y; cl_x1 = x + w; cl_y1 = y + h;
}

void fb_reset_clip(void)
{
    cl_x0 = 0; cl_y0 = 0; cl_x1 = CL_BIG; cl_y1 = CL_BIG;
}

void fb_get_clip(int *x, int *y, int *w, int *h)
{
    *x = cl_x0; *y = cl_y0; *w = cl_x1 - cl_x0; *h = cl_y1 - cl_y0;
}

/* current clip window intersected with the screen */
static void clip_bounds(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = cl_x0 > 0 ? cl_x0 : 0;
    *y0 = cl_y0 > 0 ? cl_y0 : 0;
    *x1 = cl_x1 < FB_W ? cl_x1 : FB_W;
    *y1 = cl_y1 < FB_H ? cl_y1 : FB_H;
}

void fb_pixel(int x, int y, fb_px c)
{
    int x0, y0, x1, y1; clip_bounds(&x0, &y0, &x1, &y1);
    if (x >= x0 && x < x1 && y >= y0 && y < y1) fb[y * FB_W + x] = c;
}

/* clip a rect to the framebuffer AND the active clip window; 0 if fully out */
static int clip(int *x, int *y, int *w, int *h)
{
    int x0, y0, x1, y1; clip_bounds(&x0, &y0, &x1, &y1);
    if (*x < x0) { *w += *x - x0; *x = x0; }
    if (*y < y0) { *h += *y - y0; *y = y0; }
    if (*x + *w > x1) *w = x1 - *x;
    if (*y + *h > y1) *h = y1 - *y;
    return (*w > 0 && *h > 0);
}

void fb_clear(fb_px c)
{
    int i;
    for (i = 0; i < FB_W * FB_H; i++) fb[i] = c;
}

void fb_fill_rect(int x, int y, int w, int h, fb_px c)
{
    int r, j;
    if (!clip(&x, &y, &w, &h)) return;
    for (r = 0; r < h; r++) {
        fb_px *p = &fb[(y + r) * FB_W + x];
        for (j = 0; j < w; j++) p[j] = c;
    }
}

void fb_hline(int x, int y, int w, fb_px c) { fb_fill_rect(x, y, w, 1, c); }
void fb_vline(int x, int y, int h, fb_px c) { fb_fill_rect(x, y, 1, h, c); }

void fb_frame_rect(int x, int y, int w, int h, fb_px c)
{
    if (w <= 0 || h <= 0) return;
    fb_hline(x, y, w, c);
    fb_hline(x, y + h - 1, w, c);
    fb_vline(x, y, h, c);
    fb_vline(x + w - 1, y, h, c);
}

void fb_invert_rect(int x, int y, int w, int h)
{
    int r, j;
    if (!clip(&x, &y, &w, &h)) return;
    for (r = 0; r < h; r++) {
        fb_px *p = &fb[(y + r) * FB_W + x];
        for (j = 0; j < w; j++) p[j] ^= 0x00FFFFFFu;   /* invert RGB, keep A */
    }
}

/* ---- alpha blending + gradients (for modern/"Aurora"-style chrome) --------
 * Additive + default-safe: no existing caller is touched. Pixels are
 * 0xFFBBGGRR (see FB_RGB); alpha `a` is 0..255 (0 = keep dst, 255 = src). */
/* round(x / 255) for x in 0..65025, no division. The modern chrome is
 * alpha-blend heavy, so replacing the three per-pixel integer divides in the
 * blend path with a multiply+shift is a large, broad speedup. */
static inline unsigned div255(unsigned x) { return (x * 257u + 257u) >> 16; }

static fb_px blend_px(fb_px d, fb_px s, int a)
{
    unsigned dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
    unsigned sr = s & 0xFF, sg = (s >> 8) & 0xFF, sb = (s >> 16) & 0xFF;
    unsigned na = 255u - (unsigned)a;         /* reformulate so both terms are >= 0 */
    unsigned r = div255(dr * na + sr * (unsigned)a);
    unsigned g = div255(dg * na + sg * (unsigned)a);
    unsigned b = div255(db * na + sb * (unsigned)a);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

void fb_blend_pixel(int x, int y, fb_px c, int a)
{
    int x0, y0, x1, y1; clip_bounds(&x0, &y0, &x1, &y1);
    if (a <= 0) return;
    if (a >= 255) { if (x>=x0&&x<x1&&y>=y0&&y<y1) fb[y*FB_W+x]=c; return; }
    if (x >= x0 && x < x1 && y >= y0 && y < y1) {
        fb_px *p = &fb[y * FB_W + x]; *p = blend_px(*p, c, a);
    }
}

void fb_blend_rect(int x, int y, int w, int h, fb_px c, int a)
{
    int r, j;
    if (a >= 255) { fb_fill_rect(x, y, w, h, c); return; }
    if (a <= 0 || !clip(&x, &y, &w, &h)) return;
    for (r = 0; r < h; r++) {
        fb_px *p = &fb[(y + r) * FB_W + x];
        for (j = 0; j < w; j++) p[j] = blend_px(p[j], c, a);
    }
}

void fb_grad_v(int x, int y, int w, int h, fb_px top, fb_px bot)
{
    int tr = top & 0xFF, tg = (top>>8)&0xFF, tb = (top>>16)&0xFF;
    int br = bot & 0xFF, bg = (bot>>8)&0xFF, bb = (bot>>16)&0xFF;
    int H = h, r, j, ox = x, oy = y, ow = w, oh = h;
    if (!clip(&x, &y, &w, &h)) return;
    (void)ow; (void)oh;
    for (r = 0; r < h; r++) {
        int sy = (y + r) - oy;                       /* row within the full band */
        unsigned t = H > 1 ? (unsigned)(sy * 255) / (unsigned)(H - 1) : 0;
        unsigned nt = 255u - t;
        int cr = (int)div255((unsigned)tr * nt + (unsigned)br * t);
        int cg = (int)div255((unsigned)tg * nt + (unsigned)bg * t);
        int cb = (int)div255((unsigned)tb * nt + (unsigned)bb * t);
        fb_px c = 0xFF000000u | ((fb_px)cb << 16) | ((fb_px)cg << 8) | (fb_px)cr;
        fb_px *p = &fb[(y + r) * FB_W + x];
        for (j = 0; j < w; j++) p[j] = c;
    }
    (void)ox;
}

/* ---- anti-aliased rounded rectangles (modern chrome) --------------------- *
 * `corners` is a bitmask (FB_CORNER_*); alpha 0..255. Corner arcs fade via
 * coverage blend, so rounded surfaces sit cleanly on any background. */
static void round_corner(int bx, int by, int rad, int cx_left, int cy_top,
                         fb_px c, int alpha)
{
    int R = 2*rad, R2 = R*R, band = 2*R + 1, i, j;
    for (j = 0; j < rad; j++) for (i = 0; i < rad; i++) {
        int AX = cx_left ? (2*i+1) : (R-2*i-1);
        int AY = cy_top  ? (2*j+1) : (R-2*j-1);
        int d2 = AX*AX + AY*AY, cov, a2;
        if (d2 <= R2 - band) cov = 255;
        else if (d2 >= R2 + band) cov = 0;
        else cov = 255*(R2 + band - d2)/(2*band);
        if (cov <= 0) continue;
        a2 = (alpha >= 255) ? cov : (int)div255((unsigned)cov * (unsigned)alpha);
        fb_blend_pixel(bx+i, by+j, c, a2);
    }
}

void fb_round_rect_a(int x, int y, int w, int h, int rad, fb_px c, int a, int corners)
{
    if (w <= 0 || h <= 0) return;
    if (rad < 0) rad = 0;
    if (rad > w/2) rad = w/2;
    if (rad > h/2) rad = h/2;
    fb_blend_rect(x+rad, y, w-2*rad, h, c, a);
    fb_blend_rect(x, y+rad, rad, h-2*rad, c, a);
    fb_blend_rect(x+w-rad, y+rad, rad, h-2*rad, c, a);
    if (rad == 0) return;
    if (corners & FB_CORNER_TL) round_corner(x,       y,       rad,0,0,c,a); else fb_blend_rect(x,       y,       rad,rad,c,a);
    if (corners & FB_CORNER_TR) round_corner(x+w-rad, y,       rad,1,0,c,a); else fb_blend_rect(x+w-rad, y,       rad,rad,c,a);
    if (corners & FB_CORNER_BL) round_corner(x,       y+h-rad, rad,0,1,c,a); else fb_blend_rect(x,       y+h-rad, rad,rad,c,a);
    if (corners & FB_CORNER_BR) round_corner(x+w-rad, y+h-rad, rad,1,1,c,a); else fb_blend_rect(x+w-rad, y+h-rad, rad,rad,c,a);
}

void fb_round_rect(int x, int y, int w, int h, int rad, fb_px c)
{ fb_round_rect_a(x, y, w, h, rad, c, 255, FB_CORNER_ALL); }

void fb_blend_pixel_sub(int x, int y, fb_px fg, int aR, int aG, int aB)
{
    int x0, y0, x1, y1; clip_bounds(&x0, &y0, &x1, &y1);
    if (x < x0 || x >= x1 || y < y0 || y >= y1) return;
    {
        fb_px *p = &fb[y * FB_W + x], d = *p;
        unsigned dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
        unsigned fr = fg & 0xFF, fgn = (fg >> 8) & 0xFF, fbn = (fg >> 16) & 0xFF;
        unsigned r = div255(dr * (255u - (unsigned)aR) + fr  * (unsigned)aR);
        unsigned g = div255(dg * (255u - (unsigned)aG) + fgn * (unsigned)aG);
        unsigned b = div255(db * (255u - (unsigned)aB) + fbn * (unsigned)aB);
        *p = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
}

/* optional text provider (TrueType); NULL = the built-in bitmap font */
static const fb_font *g_font = 0;
void fb_set_font(const fb_font *f) { g_font = f; }
const fb_font *fb_get_font(void) { return g_font; }

int fb_glyph(int x, int y, int ch, fb_px fg, long bg)
{
    int r, c, x0, y0, x1, y1;
    const unsigned char *g;
    if (g_font && g_font->glyph) return g_font->glyph(x, y, ch, fg, bg);
    clip_bounds(&x0, &y0, &x1, &y1);
    if (ch < UNO_FONT_FIRST || ch >= UNO_FONT_FIRST + UNO_FONT_COUNT) ch = ' ';
    g = uno_font8x8[ch - UNO_FONT_FIRST];
    for (r = 0; r < 8; r++) {
        int yy = y + r;
        unsigned char row = g[r];
        if (yy < y0 || yy >= y1) continue;
        for (c = 0; c < 8; c++) {
            int xx = x + c;
            if (xx < x0 || xx >= x1) continue;
            if (row & (0x80 >> c)) fb[yy * FB_W + xx] = fg;
            else if (bg >= 0)      fb[yy * FB_W + xx] = (fb_px)bg;
        }
    }
    return x + 8;
}

int fb_text(int x, int y, const char *s, fb_px fg, long bg)
{
    if (g_font && g_font->text) return g_font->text(x, y, s, fg, bg);
    for (; *s; s++) x = fb_glyph(x, y, (unsigned char)*s, fg, bg);
    return x;
}

int fb_text_h(void)
{
    if (g_font && g_font->height) return g_font->height();
    return 8;
}

int fb_text_w(const char *s)
{
    int n = 0;
    if (g_font && g_font->text_w) return g_font->text_w(s);
    while (*s++) n++;
    return n * 8;
}

static int fb_big_glyph(int x, int y, int ch, fb_px fg, long bg, int scale)
{
    int r, c;
    const unsigned char *g;
    if (ch < UNO_FONT_FIRST || ch >= UNO_FONT_FIRST + UNO_FONT_COUNT) ch = ' ';
    g = uno_font8x8[ch - UNO_FONT_FIRST];
    for (r = 0; r < 8; r++) {
        unsigned char row = g[r];
        for (c = 0; c < 8; c++) {
            if (row & (0x80 >> c))     fb_fill_rect(x + c * scale, y + r * scale, scale, scale, fg);
            else if (bg >= 0)          fb_fill_rect(x + c * scale, y + r * scale, scale, scale, (fb_px)bg);
        }
    }
    return x + 8 * scale;
}

int fb_big_text(int x, int y, const char *s, fb_px fg, long bg, int scale)
{
    for (; *s; s++) x = fb_big_glyph(x, y, (unsigned char)*s, fg, bg, scale);
    return x;
}

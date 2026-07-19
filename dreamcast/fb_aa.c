/* UnoDOS/Dreamcast - alpha-blend / gradient / rounded-corner framebuffer
 * primitives.
 *
 * These are what the unoui toolkit and the Aurora theme draw with; the legacy
 * Mac-style core (fb.c) never uses them, so this file is linked ONLY into the
 * unoui (`./build.sh dc uui`) build. Pure C over the shared fb[] (see fb.h),
 * ported verbatim from pc64/fb.c (via ps2/fb_aa.c) - portable software ops, no
 * platform deps; the DC's ARGB8888 fb[] has the same 0xAABBGGRR layout the ops
 * assume, so they carry over byte-identical (the DC present converts to 565).
 *
 * A settable clip window (fb_set_clip/reset) confines the AA drawing so unoui
 * can keep a widget inside its window frame. (fb.c's fill/text clip to the
 * screen only; unoui sizes its widgets to fit, so the two clip domains agree in
 * practice.) */
#include "fb.h"

/* ---- clip window (absolute fb coords); default = whole screen ------------- */
#define CL_BIG (1 << 28)
static int cl_x0 = 0, cl_y0 = 0, cl_x1 = CL_BIG, cl_y1 = CL_BIG;

void fb_set_clip(int x, int y, int w, int h)
{ cl_x0 = x; cl_y0 = y; cl_x1 = x + w; cl_y1 = y + h; }

void fb_reset_clip(void)
{ cl_x0 = 0; cl_y0 = 0; cl_x1 = CL_BIG; cl_y1 = CL_BIG; }

void fb_get_clip(int *x, int *y, int *w, int *h)
{ *x = cl_x0; *y = cl_y0; *w = cl_x1 - cl_x0; *h = cl_y1 - cl_y0; }

/* line height of the active text face - this port has only the 8x8 bitmap */
int fb_text_h(void) { return 8; }

static void clip_bounds(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = cl_x0 > 0 ? cl_x0 : 0;
    *y0 = cl_y0 > 0 ? cl_y0 : 0;
    *x1 = cl_x1 < FB_W ? cl_x1 : FB_W;
    *y1 = cl_y1 < FB_H ? cl_y1 : FB_H;
}

/* clip a rect to the framebuffer AND the active clip window; 0 if fully out */
static int aclip(int *x, int *y, int *w, int *h)
{
    int x0, y0, x1, y1; clip_bounds(&x0, &y0, &x1, &y1);
    if (*x < x0) { *w += *x - x0; *x = x0; }
    if (*y < y0) { *h += *y - y0; *y = y0; }
    if (*x + *w > x1) *w = x1 - *x;
    if (*y + *h > y1) *h = y1 - *y;
    return (*w > 0 && *h > 0);
}

void fb_pixel(int x, int y, fb_px c)
{
    int x0, y0, x1, y1; clip_bounds(&x0, &y0, &x1, &y1);
    if (x >= x0 && x < x1 && y >= y0 && y < y1) fb[y * FB_W + x] = c;
}

/* round(x / 255) without a divide, x in 0..65025 */
static unsigned div255(unsigned x) { return (x * 257u + 257u) >> 16; }

static fb_px blend_px(fb_px d, fb_px s, int a)
{
    unsigned dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
    unsigned sr = s & 0xFF, sg = (s >> 8) & 0xFF, sb = (s >> 16) & 0xFF;
    unsigned na = 255u - (unsigned)a;
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
    if (a >= 255) {
        if (!aclip(&x, &y, &w, &h)) return;
        for (r = 0; r < h; r++) { fb_px *p = &fb[(y+r)*FB_W + x]; for (j=0;j<w;j++) p[j]=c; }
        return;
    }
    if (a <= 0 || !aclip(&x, &y, &w, &h)) return;
    for (r = 0; r < h; r++) {
        fb_px *p = &fb[(y + r) * FB_W + x];
        for (j = 0; j < w; j++) p[j] = blend_px(p[j], c, a);
    }
}

void fb_grad_v(int x, int y, int w, int h, fb_px top, fb_px bot)
{
    int tr = top & 0xFF, tg = (top>>8)&0xFF, tb = (top>>16)&0xFF;
    int br = bot & 0xFF, bg = (bot>>8)&0xFF, bb = (bot>>16)&0xFF;
    int H = h, r, j, oy = y;
    if (!aclip(&x, &y, &w, &h)) return;
    for (r = 0; r < h; r++) {
        int sy = (y + r) - oy;
        unsigned t = H > 1 ? (unsigned)(sy * 255) / (unsigned)(H - 1) : 0;
        unsigned nt = 255u - t;
        int cr = (int)div255((unsigned)tr * nt + (unsigned)br * t);
        int cg = (int)div255((unsigned)tg * nt + (unsigned)bg * t);
        int cb = (int)div255((unsigned)tb * nt + (unsigned)bb * t);
        fb_px c = 0xFF000000u | ((fb_px)cb << 16) | ((fb_px)cg << 8) | (fb_px)cr;
        fb_px *p = &fb[(y + r) * FB_W + x];
        for (j = 0; j < w; j++) p[j] = c;
    }
}

/* anti-aliased rounded-corner coverage blend */
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

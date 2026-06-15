/* ===========================================================================
 * uno3d software backend - a portable triangle rasteriser into the UnoDOS
 * framebuffer (`fb`, from the port's fb.h). Needs nothing but a 32-bit
 * framebuffer, so it is the universal fallback: every port already has `fb`, so
 * this backend brings 3D to machines with no 3D hardware at all (Amiga, Mac,
 * Apple II-class), and is the reference the hardware backends are checked
 * against. It is also what a PC build would fall back to when no GPU/Direct3D is
 * present.
 *
 * Gouraud-shaded, z-buffered, half-space (edge-function) rasterisation. Affine
 * colour interpolation (no perspective-correct divide per pixel) - fine for the
 * flat-shaded/solid look UnoDOS 3D apps use.
 * ======================================================================== */
#include "uno3d_backend.h"
#include "fb.h"
#include <math.h>

static float g_zbuf[FB_W * FB_H];

static int soft_init(int w, int h) { (void)w; (void)h; return 0; }
static void soft_shutdown(void) { }

static void soft_clear(unsigned char r, unsigned char g, unsigned char b)
{
    fb_px c = FB_RGB(r, g, b);
    int i, n = FB_W * FB_H;
    for (i = 0; i < n; i++) { fb[i] = c; g_zbuf[i] = 1.0f; }
}

static float edge(const u3d_sv *a, const u3d_sv *b, float px, float py)
{
    return (b->sx - a->sx) * (py - a->sy) - (b->sy - a->sy) * (px - a->sx);
}

static void soft_tri(const u3d_stri *t)
{
    const u3d_sv *v0 = &t->v[0], *v1 = &t->v[1], *v2 = &t->v[2];
    float area = edge(v0, v1, v2->sx, v2->sy);
    int minx, maxx, miny, maxy, px, py;
    float inv;
    if (area == 0.0f) return;
    inv = 1.0f / area;

    minx = (int)floorf(v0->sx < v1->sx ? (v0->sx < v2->sx ? v0->sx : v2->sx)
                                       : (v1->sx < v2->sx ? v1->sx : v2->sx));
    maxx = (int)ceilf (v0->sx > v1->sx ? (v0->sx > v2->sx ? v0->sx : v2->sx)
                                       : (v1->sx > v2->sx ? v1->sx : v2->sx));
    miny = (int)floorf(v0->sy < v1->sy ? (v0->sy < v2->sy ? v0->sy : v2->sy)
                                       : (v1->sy < v2->sy ? v1->sy : v2->sy));
    maxy = (int)ceilf (v0->sy > v1->sy ? (v0->sy > v2->sy ? v0->sy : v2->sy)
                                       : (v1->sy > v2->sy ? v1->sy : v2->sy));
    if (minx < 0) minx = 0;
    if (maxx > FB_W - 1) maxx = FB_W - 1;
    if (miny < 0) miny = 0;
    if (maxy > FB_H - 1) maxy = FB_H - 1;

    for (py = miny; py <= maxy; py++) {
        float fy = (float)py + 0.5f;
        for (px = minx; px <= maxx; px++) {
            float fx = (float)px + 0.5f;
            float w0 = edge(v1, v2, fx, fy) * inv;
            float w1 = edge(v2, v0, fx, fy) * inv;
            float w2 = edge(v0, v1, fx, fy) * inv;
            float z;
            int idx;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;   /* outside */
            z = w0 * v0->z + w1 * v1->z + w2 * v2->z;
            idx = py * FB_W + px;
            if (z >= g_zbuf[idx]) continue;                      /* depth test */
            g_zbuf[idx] = z;
            {
                int r = (int)(w0 * v0->r + w1 * v1->r + w2 * v2->r);
                int g = (int)(w0 * v0->g + w1 * v1->g + w2 * v2->g);
                int b = (int)(w0 * v0->b + w1 * v1->b + w2 * v2->b);
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                fb[idx] = FB_RGB(r, g, b);
            }
        }
    }
}

static void soft_flush(void) { }
static void soft_present(void) { }   /* the platform glue blits `fb` itself */

const u3d_backend u3d_backend_soft = {
    "soft",
    U3D_CAP_ZBUFFER | U3D_CAP_GOURAUD,
    soft_init, soft_shutdown, soft_clear, soft_tri, soft_flush, soft_present
};

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

/* pc64 has a RUNTIME framebuffer size (FB_W/FB_H are variables), so the
   z-buffer is sized to the ceiling and indexed with the live stride. Other
   ports keep the exact static array. */
#ifdef UNO_PC64
static float g_zbuf[FB_BUF_PIX];
#else
static float g_zbuf[FB_W * FB_H];
#endif

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

/* Incremental half-space rasteriser. The three edge functions are affine in
 * (px,py), so along a scanline each steps by a constant dE/dx; z and the r/g/b
 * channels are barycentric-linear too and step the same way. So the per-pixel
 * inner loop becomes three adds + a sign test (plus stepped z/colour), instead
 * of recomputing edge() from scratch three times per pixel, the per-pixel
 * `* inv` normalisation, and the 3+9 muls for z and colour. Each edge (and z /
 * r / g / b) is evaluated once at the bounding-box min corner, then stepped by
 * its constant dE/dy down rows and dE/dx across pixels. The 1/area factor is
 * folded into the colour/z deltas and the seed (no per-pixel `* inv`), and the
 * coverage test uses only the edge SIGN (normalised by sign(area) so "inside"
 * is simply all >= 0) - exactly the old `w_i < 0` reject with the wasted
 * multiply removed. A per-row analytic span [xs,xe] then bounds the scanline to
 * the covered pixels, so a sliver triangle whose bounding box dwarfs its
 * coverage no longer costs a full-box scan; the accumulators are still advanced
 * across the row by pure addition (never a multiply jump), so every covered
 * pixel sees bit-for-bit the value a plain minx..maxx scan would - which keeps
 * silhouette and shared edges free of rounding cracks. Output pixels, rounding
 * and fill rule are the same as the brute-force version. */
static void soft_tri(const u3d_stri *t)
{
    const u3d_sv *v0 = &t->v[0], *v1 = &t->v[1], *v2 = &t->v[2];
    float area = edge(v0, v1, v2->sx, v2->sy);
    int minx, maxx, miny, maxy, py;
    float inv, invn, sgn, fx0, fy0;
    float d0x, d1x, d2x, d0y, d1y, d2y;   /* per-edge dE/dx, dE/dy (sign-normalised) */
    float dzx, drx, dgx, dbx;             /* per-pixel z/colour x-deltas (1/area in) */
    float dzy, dry, dgy, dby;             /* per-row   z/colour y-deltas (1/area in) */
    /* row-start accumulators, seeded at the min corner and stepped by the y-deltas */
    float e0r, e1r, e2r, zr, rr, gr, br;
    if (area == 0.0f) return;
    inv = 1.0f / area;
    sgn = (area < 0.0f) ? -1.0f : 1.0f;   /* fold into coverage so test is >= 0 */
    invn = sgn * inv;                     /* = 1/|area|; weight = f_i * invn     */

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
    if (minx > maxx || miny > maxy) return;

    /* per-edge gradients: dE/dx = a.sy - b.sy, dE/dy = b.sx - a.sx for edge(a,b),
       sign-normalised, using the reference's pairing (E0=edge(v1,v2) weights v0). */
    d0x = (v1->sy - v2->sy) * sgn;  d0y = (v2->sx - v1->sx) * sgn;   /* edge(v1,v2) */
    d1x = (v2->sy - v0->sy) * sgn;  d1y = (v0->sx - v2->sx) * sgn;   /* edge(v2,v0) */
    d2x = (v0->sy - v1->sy) * sgn;  d2y = (v1->sx - v0->sx) * sgn;   /* edge(v0,v1) */
    /* z / colour deltas, with 1/area (invn, on the sign-normalised edges) folded in */
    dzx = (d0x * v0->z + d1x * v1->z + d2x * v2->z) * invn;
    dzy = (d0y * v0->z + d1y * v1->z + d2y * v2->z) * invn;
    drx = (d0x * v0->r + d1x * v1->r + d2x * v2->r) * invn;
    dry = (d0y * v0->r + d1y * v1->r + d2y * v2->r) * invn;
    dgx = (d0x * v0->g + d1x * v1->g + d2x * v2->g) * invn;
    dgy = (d0y * v0->g + d1y * v1->g + d2y * v2->g) * invn;
    dbx = (d0x * v0->b + d1x * v1->b + d2x * v2->b) * invn;
    dby = (d0y * v0->b + d1y * v1->b + d2y * v2->b) * invn;

    /* seed the row accumulators once at the (clamped) min corner sample */
    fx0 = (float)minx + 0.5f;
    fy0 = (float)miny + 0.5f;
    e0r = edge(v1, v2, fx0, fy0) * sgn;
    e1r = edge(v2, v0, fx0, fy0) * sgn;
    e2r = edge(v0, v1, fx0, fy0) * sgn;
    zr = (e0r * v0->z + e1r * v1->z + e2r * v2->z) * invn;
    rr = (e0r * v0->r + e1r * v1->r + e2r * v2->r) * invn;
    gr = (e0r * v0->g + e1r * v1->g + e2r * v2->g) * invn;
    br = (e0r * v0->b + e1r * v1->b + e2r * v2->b) * invn;

    /* Row accumulators advance in the for-increment clause, so a `continue`
       (empty row) still steps them for the next row. */
    for (py = miny; py <= maxy; py++,
            e0r += d0y, e1r += d1y, e2r += d2y,
            zr  += dzy, rr  += dry, gr  += dgy, br += dby) {
        float e0 = e0r, e1 = e1r, e2 = e2r;
        float z = zr, cr = rr, cg = gr, cb = br;
        float xl = (float)minx, xr = (float)maxx;
        int xs, xe, rowbase, px;

        /* per-row early-out: solve e(x) = e_row + (x-minx)*dEdx >= 0 for the
           covered span [xs,xe] and iterate only that. A 1px guard keeps edge
           rounding from clipping a covered pixel; every pixel in the span is
           still sign-tested below, so the fill rule is unchanged. The
           accumulators are advanced to xs by PURE ADDITION (not a multiply
           jump), so the value reaching a covered edge is bit-for-bit what a
           full minx..maxx scan would compute - which keeps silhouette and
           shared edges faithful to the brute-force rasteriser (no cracks). */
        if      (d0x > 0.0f) { float b = (float)minx - e0 / d0x; if (b > xl) xl = b; }
        else if (d0x < 0.0f) { float b = (float)minx - e0 / d0x; if (b < xr) xr = b; }
        else if (e0 < 0.0f) continue;               /* whole row outside edge 0 */
        if      (d1x > 0.0f) { float b = (float)minx - e1 / d1x; if (b > xl) xl = b; }
        else if (d1x < 0.0f) { float b = (float)minx - e1 / d1x; if (b < xr) xr = b; }
        else if (e1 < 0.0f) continue;
        if      (d2x > 0.0f) { float b = (float)minx - e2 / d2x; if (b > xl) xl = b; }
        else if (d2x < 0.0f) { float b = (float)minx - e2 / d2x; if (b < xr) xr = b; }
        else if (e2 < 0.0f) continue;

        xs = (int)floorf(xl) - 1; if (xs < minx) xs = minx;
        xe = (int)ceilf (xr) + 1; if (xe > maxx) xe = maxx;
        if (xs > xe) continue;

        /* skip leading outside pixels by the same additions a full scan would do */
        for (px = minx; px < xs; px++) {
            e0 += d0x; e1 += d1x; e2 += d2x;
            z += dzx; cr += drx; cg += dgx; cb += dbx;
        }
        rowbase = py * FB_W;

        for (; px <= xe; px++) {
            if (e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f) {   /* inside */
                int idx = rowbase + px;
                if (z < g_zbuf[idx]) {                      /* depth test */
                    int r = (int)cr, g = (int)cg, b = (int)cb;
                    g_zbuf[idx] = z;
                    if (r > 255) r = 255;
                    if (g > 255) g = 255;
                    if (b > 255) b = 255;
                    fb[idx] = FB_RGB(r, g, b);
                }
            }
            e0 += d0x; e1 += d1x; e2 += d2x;
            z += dzx; cr += drx; cg += dgx; cb += dbx;
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

/* ===========================================================================
 * UNOAUTOMATE screen grab - QOI encoder over the software framebuffer.
 *
 * See unoauto_screen.h. This is a self-contained QOI *encoder* (unomedia ships
 * decoders only, and is a different lane - AGENTS.md §1/§2, so we do not reach
 * into it). QOI: "qoif" magic, big-endian dims, then a byte stream of ops with
 * a 64-entry running-colour hash + a run length. Spec mirrored from the decoder
 * in unomedia/um_qoi.c. The whole file is UNO_DEBUG-only, like URC.
 *
 * The encoder is factored into an incremental (begin / per-pixel / end) form so
 * BOTH the full keyframe grab and the delta strip feed the identical op stream -
 * the wire bytes a full grab emits are byte-for-byte what they were before this
 * split, so the C#/Python QOI decoders and the pixel-exact host tests are
 * unchanged. Delta dirty-detection is per-tile FNV hashing against a snapshot
 * kept in this file (g_tileh), so there is no multi-MB previous-frame buffer.
 * ======================================================================== */
#include "unoauto_screen.h"

#ifdef UNO_DEBUG

#include "fb.h"          /* fb[], uno_fb_w/uno_fb_h, FB_W/FB_H */
#include <stdint.h>

void uno_screen_size(int *w, int *h)
{
    if (w) *w = FB_W;
    if (h) *h = FB_H;
}

/* QOI opcodes (see the spec / um_qoi.c). */
#define QOI_OP_INDEX 0x00   /* 00xxxxxx */
#define QOI_OP_DIFF  0x40   /* 01xxxxxx */
#define QOI_OP_LUMA  0x80   /* 10xxxxxx */
#define QOI_OP_RUN   0xc0   /* 11xxxxxx */
#define QOI_OP_RGB   0xfe
#define QOI_OP_RGBA  0xff

typedef struct { unsigned char r, g, b, a; } qpx;

static int qhash(qpx p)      /* running-array index */
{
    return (p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11) & 63;
}

/* ---- incremental QOI encoder --------------------------------------------- */
typedef struct {
    unsigned char *out;
    int   cap, o, run;
    qpx   prev;
    qpx   index[64];
} qenc;

/* Write the QOI header and reset encoder state. Returns 0, or -1 if `cap` can't
 * even hold the 14-byte header plus the 8-byte end marker. */
static int q_begin(qenc *e, unsigned char *out, int cap, int ew, int eh)
{
    int i;
    if (cap < 14 + 8) return -1;
    e->out = out; e->cap = cap; e->o = 0; e->run = 0;
    e->prev.r = e->prev.g = e->prev.b = 0; e->prev.a = 255;
    for (i = 0; i < 64; i++) { e->index[i].r = e->index[i].g = e->index[i].b = e->index[i].a = 0; }
    out[e->o++] = 'q'; out[e->o++] = 'o'; out[e->o++] = 'i'; out[e->o++] = 'f';
    out[e->o++] = (unsigned char)(ew >> 24); out[e->o++] = (unsigned char)(ew >> 16);
    out[e->o++] = (unsigned char)(ew >> 8);  out[e->o++] = (unsigned char)ew;
    out[e->o++] = (unsigned char)(eh >> 24); out[e->o++] = (unsigned char)(eh >> 16);
    out[e->o++] = (unsigned char)(eh >> 8);  out[e->o++] = (unsigned char)eh;
    out[e->o++] = 4;    /* channels  */
    out[e->o++] = 0;    /* colorspace */
    return 0;
}

/* Emit one pixel. Returns 0, or -1 on buffer overflow (8 bytes of headroom for
 * the largest single op + the end marker, matching the original encoder). */
static int q_px(qenc *e, qpx px)
{
    if (e->o + 8 > e->cap) return -1;
    if (px.r == e->prev.r && px.g == e->prev.g && px.b == e->prev.b && px.a == e->prev.a) {
        e->run++;
        if (e->run == 62) { e->out[e->o++] = (unsigned char)(QOI_OP_RUN | (e->run - 1)); e->run = 0; }
        return 0;
    }
    {
        int ip;
        if (e->run > 0) { e->out[e->o++] = (unsigned char)(QOI_OP_RUN | (e->run - 1)); e->run = 0; }
        ip = qhash(px);
        if (e->index[ip].r == px.r && e->index[ip].g == px.g &&
            e->index[ip].b == px.b && e->index[ip].a == px.a) {
            e->out[e->o++] = (unsigned char)(QOI_OP_INDEX | ip);
        } else {
            e->index[ip] = px;
            if (px.a == e->prev.a) {
                int vr = (int)px.r - (int)e->prev.r;
                int vg = (int)px.g - (int)e->prev.g;
                int vb = (int)px.b - (int)e->prev.b;
                int dr_dg = vr - vg, db_dg = vb - vg;
                if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
                    e->out[e->o++] = (unsigned char)(QOI_OP_DIFF |
                        ((vr + 2) << 4) | ((vg + 2) << 2) | (vb + 2));
                } else if (dr_dg > -9 && dr_dg < 8 && vg > -33 && vg < 32 &&
                           db_dg > -9 && db_dg < 8) {
                    e->out[e->o++] = (unsigned char)(QOI_OP_LUMA | (vg + 32));
                    e->out[e->o++] = (unsigned char)(((dr_dg + 8) << 4) | (db_dg + 8));
                } else {
                    e->out[e->o++] = QOI_OP_RGB;
                    e->out[e->o++] = px.r; e->out[e->o++] = px.g; e->out[e->o++] = px.b;
                }
            } else {
                e->out[e->o++] = QOI_OP_RGBA;
                e->out[e->o++] = px.r; e->out[e->o++] = px.g; e->out[e->o++] = px.b; e->out[e->o++] = px.a;
            }
        }
        e->prev = px;
    }
    return 0;
}

/* Flush a trailing run and the 8-byte end marker. Returns the total byte count,
 * or -1 on overflow. */
static int q_end(qenc *e)
{
    if (e->run > 0) {
        if (e->o + 8 > e->cap) return -1;
        e->out[e->o++] = (unsigned char)(QOI_OP_RUN | (e->run - 1));
        e->run = 0;
    }
    if (e->o + 8 > e->cap) return -1;
    e->out[e->o++] = 0; e->out[e->o++] = 0; e->out[e->o++] = 0; e->out[e->o++] = 0;
    e->out[e->o++] = 0; e->out[e->o++] = 0; e->out[e->o++] = 0; e->out[e->o++] = 1;
    return e->o;
}

/* fb[] emitted pixel at (x,y) under `scale` (nearest-neighbour downsample). */
static qpx emit_px(int x, int y, int scale)
{
    fb_px v = fb[(y * scale) * FB_W + x * scale];
    qpx p;
    p.r = (unsigned char)(v & 0xff);
    p.g = (unsigned char)((v >> 8) & 0xff);
    p.b = (unsigned char)((v >> 16) & 0xff);
    p.a = (unsigned char)((v >> 24) & 0xff);
    return p;
}

/* ---- full keyframe grab -------------------------------------------------- */
static void scr_snapshot(int scale, int ew, int eh);   /* fwd */

int uno_screen_grab_qoi(int scale, unsigned char *out, int cap, int *ow, int *oh)
{
    int W = FB_W, H = FB_H, x, y, ew, eh;
    qenc e;

    if (scale < 1) scale = 1;
    ew = W / scale; eh = H / scale;
    if (ew < 1) ew = 1;
    if (eh < 1) eh = 1;
    if (ow) *ow = ew;
    if (oh) *oh = eh;

    if (q_begin(&e, out, cap, ew, eh) < 0) return -1;
    for (y = 0; y < eh; y++) {
        const fb_px *row = &fb[(y * scale) * FB_W];
        for (x = 0; x < ew; x++) {
            fb_px v = row[x * scale];
            qpx px;
            px.r = (unsigned char)(v & 0xff);
            px.g = (unsigned char)((v >> 8) & 0xff);
            px.b = (unsigned char)((v >> 16) & 0xff);
            px.a = (unsigned char)((v >> 24) & 0xff);
            if (q_px(&e, px) < 0) return -1;
        }
    }
    {
        int tot = q_end(&e);
        if (tot < 0) return -1;
        scr_snapshot(scale, ew, eh);   /* baseline for subsequent deltas */
        return tot;
    }
}

/* ---- delta grab: per-tile hash snapshot ---------------------------------- */
#ifdef FB_MAX_W
#  define SCR_MAX_W FB_MAX_W
#  define SCR_MAX_H FB_MAX_H
#else
#  define SCR_MAX_W FB_W
#  define SCR_MAX_H FB_H
#endif
#define SCR_TILE     UNO_SCREEN_TILE
#define SCR_MAXCOLS  ((SCR_MAX_W + SCR_TILE - 1) / SCR_TILE)
#define SCR_MAXROWS  ((SCR_MAX_H + SCR_TILE - 1) / SCR_TILE)
#define SCR_MAXTILES (SCR_MAXCOLS * SCR_MAXROWS)

static unsigned       g_tileh[SCR_MAXTILES];   /* per-tile FNV hash snapshot     */
static unsigned short g_chg[SCR_MAXTILES];     /* changed tile indices, one pass */
static int            g_have_snap;             /* a valid snapshot exists        */
static int            g_snap_scale, g_snap_ew, g_snap_eh;   /* what it was taken at */

/* FNV-1a over a tile's emitted pixels (folds the whole 32-bit fb word). A hash
 * collision would drop a real change (a stale tile) - astronomically unlikely
 * for a debug view, and self-heals on the next differing frame. */
static unsigned tile_hash(int tc, int tr, int scale, int ew, int eh)
{
    int x0 = tc * SCR_TILE, x1 = x0 + SCR_TILE;
    int y0 = tr * SCR_TILE, y1 = y0 + SCR_TILE;
    unsigned h = 2166136261u;
    int x, y;
    if (x1 > ew) x1 = ew;
    if (y1 > eh) y1 = eh;
    for (y = y0; y < y1; y++) {
        int base = (y * scale) * FB_W;
        for (x = x0; x < x1; x++) h = (h ^ (unsigned)fb[base + x * scale]) * 16777619u;
    }
    return h;
}

/* Recompute the whole snapshot for the current frame. */
static void scr_snapshot(int scale, int ew, int eh)
{
    int cols = (ew + SCR_TILE - 1) / SCR_TILE;
    int rows = (eh + SCR_TILE - 1) / SCR_TILE;
    int tr, tc;
    for (tr = 0; tr < rows; tr++)
        for (tc = 0; tc < cols; tc++)
            g_tileh[tr * cols + tc] = tile_hash(tc, tr, scale, ew, eh);
    g_have_snap = 1; g_snap_scale = scale; g_snap_ew = ew; g_snap_eh = eh;
}

int uno_screen_grab_delta(int scale, unsigned char *out, int cap,
                          int *ow, int *oh, int *ocols, int *otw, int *oth,
                          int *onch, int *ostrip)
{
    int W = FB_W, H = FB_H, ew, eh, cols, rows, tr, tc, nch = 0, i, strip, stripcap;

    if (scale < 1) scale = 1;
    ew = W / scale; eh = H / scale;
    if (ew < 1) ew = 1;
    if (eh < 1) eh = 1;
    cols = (ew + SCR_TILE - 1) / SCR_TILE;
    rows = (eh + SCR_TILE - 1) / SCR_TILE;
    if (ow) *ow = ew; if (oh) *oh = eh; if (ocols) *ocols = cols;
    if (otw) *otw = SCR_TILE; if (oth) *oth = SCR_TILE;
    if (onch) *onch = 0; if (ostrip) *ostrip = 0;

    /* Need a snapshot taken at the SAME scale/size to diff against; otherwise
     * establish one now and tell the caller to send a keyframe this round. */
    if (!g_have_snap || g_snap_scale != scale || g_snap_ew != ew || g_snap_eh != eh) {
        scr_snapshot(scale, ew, eh);
        return -1;
    }

    /* Reserve room for the worst-case manifest (every tile changed) so the strip
     * encode can never crowd it out. */
    stripcap = cap - cols * rows * 2;
    if (stripcap < 14 + 8) { scr_snapshot(scale, ew, eh); return -1; }

    /* Collect changed tiles, refreshing their hashes in place. Unchanged tiles
     * already match, so after a clean pass the snapshot is fully current. */
    for (tr = 0; tr < rows; tr++) {
        for (tc = 0; tc < cols; tc++) {
            unsigned h = tile_hash(tc, tr, scale, ew, eh);
            int t = tr * cols + tc;
            if (h != g_tileh[t]) {
                g_tileh[t] = h;
                if (nch >= SCR_MAXTILES) { scr_snapshot(scale, ew, eh); return -1; }
                g_chg[nch++] = (unsigned short)t;
            }
        }
    }
    if (onch) *onch = nch;
    if (nch == 0) return 0;                 /* static frame: empty payload */

    /* Encode the changed tiles as one vertical strip (width SCR_TILE, height
     * nch*SCR_TILE), fed straight from fb[] in strip order - no scratch copy. */
    {
        qenc e;
        if (q_begin(&e, out, stripcap, SCR_TILE, nch * SCR_TILE) < 0) {
            scr_snapshot(scale, ew, eh); return -1;
        }
        for (i = 0; i < nch; i++) {
            int t = g_chg[i], tcc = t % cols, trr = t / cols;
            int bx = tcc * SCR_TILE, by = trr * SCR_TILE, cr;
            for (cr = 0; cr < SCR_TILE; cr++) {
                int yy = by + cr, x;
                for (x = 0; x < SCR_TILE; x++) {
                    int sx = bx + x;
                    qpx p;
                    if (sx < ew && yy < eh) {
                        p = emit_px(sx, yy, scale);
                    } else {
                        p.r = p.g = p.b = 0; p.a = 0;   /* pad partial edge tiles */
                    }
                    if (q_px(&e, p) < 0) { scr_snapshot(scale, ew, eh); return -1; }
                }
            }
        }
        strip = q_end(&e);
        if (strip < 0) { scr_snapshot(scale, ew, eh); return -1; }
    }

    /* Append the manifest: nch tile indices, u16 little-endian, after the strip. */
    {
        int mo = strip;
        for (i = 0; i < nch; i++) {
            out[mo++] = (unsigned char)(g_chg[i] & 0xff);
            out[mo++] = (unsigned char)((g_chg[i] >> 8) & 0xff);
        }
        if (ostrip) *ostrip = strip;
        return mo;
    }
}

#endif /* UNO_DEBUG */

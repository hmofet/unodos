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
 * every path feeds the identical op stream - a full grab's wire bytes are
 * byte-for-byte what they were before delta streaming, so the C#/Python QOI
 * decoders and the pixel-exact host tests are unchanged. Three consumers share
 * it: the full keyframe grab, the delta grab (only the tiles that changed since
 * the previous grab, per-tile FNV hashing against a snapshot kept here - no
 * multi-MB previous-frame buffer, since fb[] is up to 1920x1200), and the
 * server-side session recorder (captures keyframe+delta frames into a RAM ring
 * on the shell tick, with its OWN snapshot so it never disturbs the live view).
 * ======================================================================== */
#include "unoauto_screen.h"

#ifdef UNO_DEBUG

#include "fb.h"          /* fb[], uno_fb_w/uno_fb_h, FB_W/FB_H */
#include <stdint.h>

unsigned long long uno_dbg_uptime_ms(void);   /* uno_debug.c: capture clock */

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

/* ---- geometry / tiling --------------------------------------------------- */
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

/* Fill `snaph` with every tile's current hash. */
static void snap_fill(unsigned *snaph, int scale, int ew, int eh)
{
    int cols = (ew + SCR_TILE - 1) / SCR_TILE;
    int rows = (eh + SCR_TILE - 1) / SCR_TILE;
    int tr, tc;
    for (tr = 0; tr < rows; tr++)
        for (tc = 0; tc < cols; tc++)
            snaph[tr * cols + tc] = tile_hash(tc, tr, scale, ew, eh);
}

/* Diff the frame against snapshot `snaph`, refreshing changed hashes in place
 * and appending their row-major indices to `chg`. Returns the count, or -1 if it
 * would exceed `maxch`. */
static int scr_diff(unsigned *snaph, unsigned short *chg, int maxch,
                    int scale, int ew, int eh)
{
    int cols = (ew + SCR_TILE - 1) / SCR_TILE;
    int rows = (eh + SCR_TILE - 1) / SCR_TILE;
    int tr, tc, nch = 0;
    for (tr = 0; tr < rows; tr++) {
        for (tc = 0; tc < cols; tc++) {
            unsigned h = tile_hash(tc, tr, scale, ew, eh);
            int t = tr * cols + tc;
            if (h != snaph[t]) {
                snaph[t] = h;
                if (nch >= maxch) return -1;
                chg[nch++] = (unsigned short)t;
            }
        }
    }
    return nch;
}

/* Encode changed tiles chg[0..nch) as one vertical QOI strip (width SCR_TILE,
 * height nch*SCR_TILE), fed straight from fb[] in strip order - no scratch copy.
 * Edge tiles are padded to a full cell (the client blits only the valid
 * sub-rect). Returns strip byte count, or -1 on overflow. */
static int scr_encode_strip(unsigned char *out, int cap, const unsigned short *chg,
                            int nch, int cols, int scale, int ew, int eh)
{
    qenc e;
    int i;
    if (q_begin(&e, out, cap, SCR_TILE, nch * SCR_TILE) < 0) return -1;
    for (i = 0; i < nch; i++) {
        int t = chg[i], tcc = t % cols, trr = t / cols;
        int bx = tcc * SCR_TILE, by = trr * SCR_TILE, cr;
        for (cr = 0; cr < SCR_TILE; cr++) {
            int yy = by + cr, x;
            for (x = 0; x < SCR_TILE; x++) {
                int sx = bx + x;
                qpx p;
                if (sx < ew && yy < eh) p = emit_px(sx, yy, scale);
                else { p.r = p.g = p.b = 0; p.a = 0; }
                if (q_px(&e, p) < 0) return -1;
            }
        }
    }
    return q_end(&e);
}

/* Encode the whole framebuffer as a QOI image; does NOT touch any snapshot.
 * Returns the byte count (or -1), and the emitted dims in oew,oeh. */
static int full_encode(int scale, unsigned char *out, int cap, int *oew, int *oeh)
{
    int W = FB_W, H = FB_H, x, y, ew, eh;
    qenc e;
    if (scale < 1) scale = 1;
    ew = W / scale; eh = H / scale;
    if (ew < 1) ew = 1;
    if (eh < 1) eh = 1;
    if (oew) *oew = ew;
    if (oeh) *oeh = eh;
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
    return q_end(&e);
}

/* ---- live-view grabs (full keyframe + delta) ----------------------------- */
static unsigned       g_live_h[SCR_MAXTILES];  /* live-view per-tile hash snapshot */
static unsigned short g_chg[SCR_MAXTILES];     /* changed tile scratch (live)       */
static int            g_live_have;             /* a valid live snapshot exists      */
static int            g_live_scale, g_live_ew, g_live_eh;   /* what it was taken at */

int uno_screen_grab_qoi(int scale, unsigned char *out, int cap, int *ow, int *oh)
{
    int ew = 0, eh = 0;
    int tot = full_encode(scale, out, cap, &ew, &eh);
    if (ow) *ow = ew;
    if (oh) *oh = eh;
    if (tot < 0) return -1;
    snap_fill(g_live_h, scale < 1 ? 1 : scale, ew, eh);      /* baseline for deltas */
    g_live_have = 1; g_live_scale = (scale < 1 ? 1 : scale); g_live_ew = ew; g_live_eh = eh;
    return tot;
}

int uno_screen_grab_delta(int scale, unsigned char *out, int cap,
                          int *ow, int *oh, int *ocols, int *otw, int *oth,
                          int *onch, int *ostrip)
{
    int W = FB_W, H = FB_H, ew, eh, cols, rows, nch, i, strip, stripcap;

    if (scale < 1) scale = 1;
    ew = W / scale; eh = H / scale;
    if (ew < 1) ew = 1;
    if (eh < 1) eh = 1;
    cols = (ew + SCR_TILE - 1) / SCR_TILE;
    rows = (eh + SCR_TILE - 1) / SCR_TILE;
    if (ow) *ow = ew; if (oh) *oh = eh; if (ocols) *ocols = cols;
    if (otw) *otw = SCR_TILE; if (oth) *oth = SCR_TILE;
    if (onch) *onch = 0; if (ostrip) *ostrip = 0;

    /* Need a snapshot at the SAME scale/size to diff against; otherwise
     * establish one now and tell the caller to send a keyframe this round. */
    if (!g_live_have || g_live_scale != scale || g_live_ew != ew || g_live_eh != eh) {
        snap_fill(g_live_h, scale, ew, eh);
        g_live_have = 1; g_live_scale = scale; g_live_ew = ew; g_live_eh = eh;
        return -1;
    }

    /* Reserve room for the worst-case manifest (every tile changed) so the strip
     * encode can never crowd it out. */
    stripcap = cap - cols * rows * 2;
    if (stripcap < 14 + 8) goto keyframe;

    nch = scr_diff(g_live_h, g_chg, SCR_MAXTILES, scale, ew, eh);
    if (nch < 0) goto keyframe;                 /* too many tiles: keyframe */
    if (onch) *onch = nch;
    if (nch == 0) return 0;                     /* static frame: empty payload */

    strip = scr_encode_strip(out, stripcap, g_chg, nch, cols, scale, ew, eh);
    if (strip < 0) goto keyframe;

    {   /* append the manifest: nch tile indices, u16 little-endian, after the strip */
        int mo = strip;
        for (i = 0; i < nch; i++) {
            out[mo++] = (unsigned char)(g_chg[i] & 0xff);
            out[mo++] = (unsigned char)((g_chg[i] >> 8) & 0xff);
        }
        if (ostrip) *ostrip = strip;
        return mo;
    }

keyframe:
    /* Refresh the snapshot fully (the partial scr_diff pass may have updated some
     * hashes) so the next delta is coherent, then let the caller send a keyframe. */
    snap_fill(g_live_h, scale, ew, eh);
    g_live_have = 1; g_live_scale = scale; g_live_ew = ew; g_live_eh = eh;
    return -1;
}

/* ---- server-side session capture ----------------------------------------- *
 * Records frames into a RAM ring on the shell tick (uno_screen_capture_tick),
 * decoupled from the client's network poll rate, with its OWN snapshot so it
 * never disturbs the live view. Each ring frame is a 12-byte header then a
 * payload:
 *     u8  type       0 = keyframe (payload = full-frame QOI)
 *                    1 = delta    (payload = [QOI strip][manifest u16-LE * nch])
 *     u8  pad
 *     u16 nch        changed tiles (delta; 0 for keyframe / static frame)
 *     u32 strip      strip byte count (keyframe: == payloadlen)
 *     u32 payloadlen bytes of payload that follow
 * ew/eh/cols/tw/th/fps are constant for a recording and reported at stop, so the
 * client reconstructs frame-by-frame the same way the live view composites. */
#define SCR_CAP_BYTES (4 * 1024 * 1024)
#define SCR_CAP_HDR   12
static unsigned char  g_cap[SCR_CAP_BYTES];
static unsigned       g_cap_h[SCR_MAXTILES];   /* capture's OWN tile-hash snapshot  */
static unsigned short g_cap_chg[SCR_MAXTILES];
static int  g_cap_on, g_cap_have;
static int  g_cap_len, g_cap_frames, g_cap_dropped;
static int  g_cap_scale, g_cap_ew, g_cap_eh, g_cap_cols, g_cap_fps;
static unsigned g_cap_interval, g_cap_next_ms;

static void cap_write_hdr(unsigned char *h, int type, int nch, int strip, int payload)
{
    h[0] = (unsigned char)type; h[1] = 0;
    h[2] = (unsigned char)(nch & 0xff);      h[3] = (unsigned char)((nch >> 8) & 0xff);
    h[4] = (unsigned char)(strip & 0xff);    h[5] = (unsigned char)((strip >> 8) & 0xff);
    h[6] = (unsigned char)((strip >> 16) & 0xff); h[7] = (unsigned char)((strip >> 24) & 0xff);
    h[8] = (unsigned char)(payload & 0xff);  h[9] = (unsigned char)((payload >> 8) & 0xff);
    h[10] = (unsigned char)((payload >> 16) & 0xff); h[11] = (unsigned char)((payload >> 24) & 0xff);
}

/* Append one captured frame to the ring; stops the recording if it won't fit or
 * the desktop size changed out from under us. */
static void cap_push_frame(void)
{
    unsigned char *base;
    int avail, cols, rows, type, nch = 0, strip = 0, payload = 0;

    if (FB_W / g_cap_scale != g_cap_ew || FB_H / g_cap_scale != g_cap_eh) {
        g_cap_on = 0; return;                  /* resolution changed mid-record */
    }
    cols = g_cap_cols;
    rows = (g_cap_eh + SCR_TILE - 1) / SCR_TILE;
    if (g_cap_len + SCR_CAP_HDR + 64 > SCR_CAP_BYTES) { g_cap_dropped++; g_cap_on = 0; return; }
    base  = g_cap + g_cap_len + SCR_CAP_HDR;
    avail = SCR_CAP_BYTES - (g_cap_len + SCR_CAP_HDR);

    if (!g_cap_have) {
        int ew, eh;
        strip = full_encode(g_cap_scale, base, avail, &ew, &eh);
        if (strip < 0) { g_cap_dropped++; g_cap_on = 0; return; }
        snap_fill(g_cap_h, g_cap_scale, g_cap_ew, g_cap_eh);
        g_cap_have = 1;
        type = 0; nch = 0; payload = strip;
    } else {
        int reserve = cols * rows * 2, scap = avail - reserve;
        if (scap < 14 + 8) { g_cap_dropped++; g_cap_on = 0; return; }
        nch = scr_diff(g_cap_h, g_cap_chg, SCR_MAXTILES, g_cap_scale, g_cap_ew, g_cap_eh);
        if (nch == 0) { type = 1; strip = 0; payload = 0; }        /* static frame */
        else if (nch < 0) goto cap_keyframe;                        /* too many tiles */
        else {
            strip = scr_encode_strip(base, scap, g_cap_chg, nch, cols,
                                     g_cap_scale, g_cap_ew, g_cap_eh);
            if (strip < 0) goto cap_keyframe;
            {   int i, mo = strip;
                for (i = 0; i < nch; i++) {
                    base[mo++] = (unsigned char)(g_cap_chg[i] & 0xff);
                    base[mo++] = (unsigned char)((g_cap_chg[i] >> 8) & 0xff);
                }
                type = 1; payload = mo;
            }
        }
    }
    cap_write_hdr(g_cap + g_cap_len, type, nch, strip, payload);
    g_cap_len += SCR_CAP_HDR + payload;
    g_cap_frames++;
    return;

cap_keyframe:
    {
        int ew, eh;
        strip = full_encode(g_cap_scale, base, avail, &ew, &eh);
        if (strip < 0) { g_cap_dropped++; g_cap_on = 0; return; }
        snap_fill(g_cap_h, g_cap_scale, g_cap_ew, g_cap_eh);
        cap_write_hdr(g_cap + g_cap_len, 0, 0, strip, strip);
        g_cap_len += SCR_CAP_HDR + strip;
        g_cap_frames++;
    }
}

int uno_screen_capture_start(int scale, int fps)
{
    if (g_cap_on) return 0;
    if (scale < 1) scale = 1;
    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;
    g_cap_scale = scale;
    g_cap_ew = FB_W / scale; g_cap_eh = FB_H / scale;
    if (g_cap_ew < 1) g_cap_ew = 1;
    if (g_cap_eh < 1) g_cap_eh = 1;
    g_cap_cols = (g_cap_ew + SCR_TILE - 1) / SCR_TILE;
    g_cap_len = 0; g_cap_frames = 0; g_cap_dropped = 0; g_cap_have = 0;
    g_cap_fps = fps; g_cap_interval = 1000u / (unsigned)fps;
    g_cap_next_ms = (unsigned)uno_dbg_uptime_ms();   /* capture the first frame now */
    g_cap_on = 1;
    return 1;
}

void uno_screen_capture_stop(void) { g_cap_on = 0; }

void uno_screen_capture_tick(void)
{
    unsigned now;
    if (!g_cap_on) return;
    now = (unsigned)uno_dbg_uptime_ms();
    if ((int)(now - g_cap_next_ms) < 0) return;
    g_cap_next_ms = now + g_cap_interval;
    cap_push_frame();
}

void uno_screen_capture_stat(int *frames, int *bytes, int *dropped, int *on,
                             int *scale, int *ew, int *eh, int *cols, int *fps)
{
    if (frames) *frames = g_cap_frames;
    if (bytes)  *bytes  = g_cap_len;
    if (dropped)*dropped = g_cap_dropped;
    if (on)     *on     = g_cap_on;
    if (scale)  *scale  = g_cap_scale;
    if (ew)     *ew     = g_cap_ew;
    if (eh)     *eh     = g_cap_eh;
    if (cols)   *cols   = g_cap_cols;
    if (fps)    *fps    = g_cap_fps;
}

int uno_screen_capture_read(int off, unsigned char *dst, int len)
{
    int i;
    if (off < 0 || off >= g_cap_len || len < 1) return 0;
    if (off + len > g_cap_len) len = g_cap_len - off;
    for (i = 0; i < len; i++) dst[i] = g_cap[off + i];
    return len;
}

#endif /* UNO_DEBUG */

/* ===========================================================================
 * unomedia - the QOI decoder (Quite OK Image), written from scratch against
 * the QOI 1.0 one-page specification.
 *
 * The complete spec: "qoif" magic, big-endian dimensions, channels 3/4 and
 * colorspace 0/1 (both informative only - decoding is identical), and all
 * six chunk kinds: QOI_OP_RGB, QOI_OP_RGBA, QOI_OP_INDEX into the 64-entry
 * running hash table ((r*3+g*5+b*7+a*11)%64), QOI_OP_DIFF (2-bit channel
 * deltas, bias 2), QOI_OP_LUMA (6-bit green delta, bias 32, red/blue coded
 * relative to it with bias 8), and QOI_OP_RUN (bias -1, lengths 1..62).
 * All delta arithmetic wraps mod 256 as the spec requires.
 *
 * The 8-byte end marker (seven 0x00 then 0x01) is TOLERATED rather than
 * demanded: once every pixel has decoded, a missing or short marker is
 * ignored - the pixels are all there and a truncated tail should not turn
 * a complete image into an error.
 *
 * Distrust: dimensions are guarded before anything downstream allocates; a
 * run that claims more pixels than remain is clamped to the image (the
 * stream never writes outside it); a stream that ends before the last
 * pixel fails with a truncation error.
 * ======================================================================== */
#include "unomedia.h"
#include <string.h>
#include <stdint.h>

/* ---- buffered forward reader over um_read ------------------------------- */
#define QOI_BUF 512
static struct {
    long pos;
    int  n, at;
    unsigned char buf[QOI_BUF];
} q_rd;

static void q_seek(long off) { q_rd.pos = off; q_rd.n = 0; q_rd.at = 0; }

static int q_byte(void)
{
    if (q_rd.at >= q_rd.n) {
        long got = um_read(q_rd.pos, q_rd.buf, QOI_BUF);
        if (got <= 0) return -1;
        q_rd.pos += got;
        q_rd.n = (int)got;
        q_rd.at = 0;
    }
    return q_rd.buf[q_rd.at++];
}

/* ---- decoder state ------------------------------------------------------- */
static int q_w, q_h;
static int q_channels;
static int q_done;

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static int qoi_probe(const unsigned char *h, long n, const char *ext)
{
    (void)ext;
    return n >= 4 && memcmp(h, "qoif", 4) == 0;
}

static int qoi_open(um_image_info *info)
{
    unsigned char h[14];
    uint32_t w, hh;

    q_done = 0;
    if (um_read(0, h, 14) != 14) {
        um_set_error("truncated QOI header");
        return 0;
    }
    if (memcmp(h, "qoif", 4) != 0) {
        um_set_error("bad QOI magic");
        return 0;
    }
    w  = be32(h + 4);
    hh = be32(h + 8);
    if (w == 0 || hh == 0 || w > UM_MAX_DIM || hh > UM_MAX_DIM ||
        (long)w * (long)hh > UM_MAX_PIXELS) {
        um_set_error("QOI dimensions out of range");
        return 0;
    }
    if (h[12] != 3 && h[12] != 4) {
        um_set_error("QOI channels must be 3 or 4");
        return 0;
    }
    if (h[13] > 1) {
        um_set_error("QOI colorspace must be 0 or 1");
        return 0;
    }
    q_w = (int)w;
    q_h = (int)hh;
    q_channels = h[12];

    info->format = "QOI";
    info->w      = q_w;
    info->h      = q_h;
    info->bpp    = q_channels * 8;
    info->alpha  = (q_channels == 4);
    info->frames = 1;
    return 1;
}

static int qoi_frame(um_px *dst, int *delay_ms)
{
    um_px index[64];
    unsigned r = 0, g = 0, b = 0, a = 255;
    long total = (long)q_w * q_h, i = 0, run = 0;

    if (q_done) return 0;
    memset(index, 0, sizeof index);
    q_seek(14);

    while (i < total) {
        int b1;
        if (run > 0) {                               /* repeat previous px   */
            run--;
            dst[i++] = UM_PX(r, g, b, a);
            continue;
        }
        b1 = q_byte();
        if (b1 < 0) { um_set_error("truncated QOI stream"); return -1; }
        if (b1 == 0xFE) {                            /* QOI_OP_RGB           */
            int nr = q_byte(), ng = q_byte(), nb = q_byte();
            if (nb < 0) { um_set_error("truncated QOI stream"); return -1; }
            r = (unsigned)nr; g = (unsigned)ng; b = (unsigned)nb;
        } else if (b1 == 0xFF) {                     /* QOI_OP_RGBA          */
            int nr = q_byte(), ng = q_byte(), nb = q_byte(), na = q_byte();
            if (na < 0) { um_set_error("truncated QOI stream"); return -1; }
            r = (unsigned)nr; g = (unsigned)ng;
            b = (unsigned)nb; a = (unsigned)na;
        } else switch (b1 >> 6) {
        case 0: {                                    /* QOI_OP_INDEX         */
            um_px px = index[b1 & 63];
            r = px & 255; g = (px >> 8) & 255;
            b = (px >> 16) & 255; a = px >> 24;
            break;
        }
        case 1:                                      /* QOI_OP_DIFF          */
            r = (r + ((unsigned)(b1 >> 4) & 3) + 254) & 255;   /* -2 bias    */
            g = (g + ((unsigned)(b1 >> 2) & 3) + 254) & 255;
            b = (b + ((unsigned)b1 & 3) + 254) & 255;
            break;
        case 2: {                                    /* QOI_OP_LUMA          */
            int b2 = q_byte();
            unsigned dg;
            if (b2 < 0) { um_set_error("truncated QOI stream"); return -1; }
            dg = ((unsigned)b1 & 63) + 224;                    /* -32 bias   */
            g = (g + dg) & 255;
            r = (r + dg + ((unsigned)(b2 >> 4) & 15) + 248) & 255; /* -8     */
            b = (b + dg + ((unsigned)b2 & 15) + 248) & 255;
            break;
        }
        default:                                     /* QOI_OP_RUN           */
            run = (b1 & 63);                         /* this px + run more   */
            break;
        }
        index[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = UM_PX(r, g, b, a);
        dst[i++] = UM_PX(r, g, b, a);
        if (run > total - i) run = total - i;        /* image bounds win     */
    }
    /* end marker deliberately not demanded - see the banner */

    q_done = 1;
    if (delay_ms) *delay_ms = 0;
    return 1;
}

static void qoi_rewind(void) { q_done = 0; }

static void qoi_close(void) { q_done = 0; }

const um_idecoder um_idec_qoi =
    { "QOI", qoi_probe, qoi_open, qoi_frame, qoi_rewind, qoi_close };

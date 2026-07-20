/* ===========================================================================
 * unomedia - the netpbm decoder (PBM/PGM/PPM, P1..P6), written from scratch
 * against the netpbm format documents.
 *
 * All six classic variants: P1/P2/P3 (plain ASCII bitmap/graymap/pixmap)
 * and P4/P5/P6 (their raw binary forms). The header tokenizer accepts
 * arbitrary whitespace and '#' comments anywhere between tokens; the same
 * tokenizer reads the ASCII rasters, where P1 digits may also abut with no
 * separator (the spec allows it and old writers do it). Binary rasters
 * start after the single whitespace byte that terminates the last header
 * number (a trailing comment there is skipped through its newline). P4
 * rows are bit-packed MSB-first and padded to a byte boundary; in P1/P4 a
 * set bit/1 is BLACK. maxval may be 1..65535 - samples are scaled to 8 bit
 * as v*255/maxval (truncating); binary 16-bit samples are big-endian per the
 * spec. Output is always opaque (alpha=0 in info, A=0xFF in pixels).
 *
 * P7 (PAM) is recognised by the probe and refused in open with a precise
 * message - "not decoded in this build" beats "not an image".
 *
 * Distrust: maxval 0 (or >65535) refused, dimensions guarded before any
 * decode, every ASCII sample checked against maxval, and any raster that
 * runs out of bytes fails with a truncation error - nothing is guessed.
 * ======================================================================== */
#include "unomedia.h"
#include <string.h>
#include <stdint.h>

/* ---- buffered forward reader over um_read ------------------------------- */
#define PNM_BUF 512
static struct {
    long pos;
    int  n, at;
    unsigned char buf[PNM_BUF];
} n_rd;

static void n_seek(long off) { n_rd.pos = off; n_rd.n = 0; n_rd.at = 0; }

static long n_tell(void) { return n_rd.pos - n_rd.n + n_rd.at; }

static int n_byte(void)
{
    if (n_rd.at >= n_rd.n) {
        long got = um_read(n_rd.pos, n_rd.buf, PNM_BUF);
        if (got <= 0) return -1;
        n_rd.pos += got;
        n_rd.n = (int)got;
        n_rd.at = 0;
    }
    return n_rd.buf[n_rd.at++];
}

/* ---- tokenizer ------------------------------------------------------------ */
static int n_isws(int c)
{ return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
         c == '\v' || c == '\f'; }

/* next significant char: skips whitespace and '#'-to-newline comments */
static int n_sig(void)
{
    int c;
    for (;;) {
        c = n_byte();
        if (c < 0) return -1;
        if (c == '#') {
            do { c = n_byte(); } while (c >= 0 && c != '\n');
            if (c < 0) return -1;
            continue;
        }
        if (!n_isws(c)) return c;
    }
}

static int n_term;                  /* char that ended the last n_uint, -1 EOF */

/* next unsigned decimal token; -1 = malformed/EOF. Values are capped at
 * 1<<28 - far past any legal dimension or maxval, so the caller's range
 * check reports the real problem without overflow on the way there. */
static long n_uint(void)
{
    int c = n_sig();
    long v = 0;
    if (c < '0' || c > '9') { n_term = c; return -1; }
    while (c >= '0' && c <= '9') {
        if (v < (1L << 28)) v = v * 10 + (c - '0');
        c = n_byte();
    }
    n_term = c;
    return v;
}

/* ---- decoder state ------------------------------------------------------- */
static int  n_type;                 /* 1..6                                  */
static int  n_w, n_h;
static long n_maxval;
static long n_data;                 /* raster start (binary formats)         */
static int  n_done;

static int pnm_probe(const unsigned char *h, long n, const char *ext)
{
    (void)ext;
    if (n < 3) return 0;
    if (h[0] != 'P' || h[1] < '1' || h[1] > '7') return 0;
    return n_isws(h[2]) || h[2] == '#';
}

static int pnm_open(um_image_info *info)
{
    long w, h, mv;
    int c;

    n_done = 0;
    n_seek(0);
    c = n_byte();
    if (c != 'P') { um_set_error("malformed PNM header"); return 0; }
    c = n_byte();
    if (c == '7') {
        um_set_error("PAM (P7) recognised - not decoded in this build");
        return 0;
    }
    if (c < '1' || c > '6') { um_set_error("malformed PNM header"); return 0; }
    n_type = c - '0';

    w = n_uint();
    h = n_uint();
    if (w < 0 || h < 0) { um_set_error("malformed PNM header"); return 0; }
    if (w == 0 || h == 0 || w > UM_MAX_DIM || h > UM_MAX_DIM ||
        w * h > UM_MAX_PIXELS) {
        um_set_error("PNM dimensions out of range");
        return 0;
    }
    n_w = (int)w; n_h = (int)h;

    if (n_type == 1 || n_type == 4) {
        mv = 1;
    } else {
        mv = n_uint();
        if (mv < 1 || mv > 65535) {
            um_set_error("PNM maxval out of range (1..65535)");
            return 0;
        }
    }
    n_maxval = mv;

    if (n_type >= 4) {
        /* the raster begins after the single whitespace that ended the last
           header number (a comment there is skipped through its newline) */
        if (n_term == '#') {
            do { c = n_byte(); } while (c >= 0 && c != '\n');
            if (c < 0) { um_set_error("truncated PNM header"); return 0; }
        } else if (!n_isws(n_term)) {
            um_set_error("missing whitespace before PNM raster");
            return 0;
        }
        n_data = n_tell();
        {   /* binary sizes are exact - check up front, fail precisely */
            long need;
            if (n_type == 4)
                need = (long)((n_w + 7) / 8) * n_h;
            else
                need = (long)n_w * n_h * (n_type == 6 ? 3 : 1) *
                       (n_maxval > 255 ? 2 : 1);
            if (um_size() - n_data < need) {
                um_set_error("truncated PNM raster");
                return 0;
            }
        }
    } else {
        n_data = n_tell() - 1;      /* re-scan from the header terminator    */
    }

    info->format = (n_type == 1 || n_type == 4) ? "PBM" :
                   (n_type == 2 || n_type == 5) ? "PGM" : "PPM";
    info->w      = n_w;
    info->h      = n_h;
    info->bpp    = (n_type == 1 || n_type == 4) ? 1 :
                   (n_maxval > 255 ? 16 : 8) *
                   ((n_type == 3 || n_type == 6) ? 3 : 1);
    info->alpha  = 0;
    info->frames = 1;
    return 1;
}

/* scale a 0..maxval sample to 0..255: v*255/maxval, truncating (identity
 * when maxval is 255; for 65535 this is v/257, matching the common Q16
 * quantum-to-char reduction) */
static unsigned n_scale(long v)
{
    if (n_maxval == 255) return (unsigned)v;
    return (unsigned)((unsigned long)v * 255 / (unsigned long)n_maxval);
}

/* one binary sample (1 or 2 bytes big-endian); -1 = truncated */
static long n_bin(void)
{
    int hi = n_byte(), lo;
    if (hi < 0) return -1;
    if (n_maxval <= 255) return hi;
    lo = n_byte();
    if (lo < 0) return -1;
    return ((long)hi << 8) | lo;
}

/* one ASCII sample, range-checked; -1 = truncated/malformed/out of range */
static long n_ascii(void)
{
    long v = n_uint();
    if (v < 0) { um_set_error("truncated PNM raster"); return -1; }
    if (v > n_maxval) { um_set_error("PNM sample exceeds maxval"); return -1; }
    return v;
}

static int pnm_frame(um_px *dst, int *delay_ms)
{
    long total = (long)n_w * n_h, i;

    if (n_done) return 0;
    n_seek(n_data);

    switch (n_type) {
    case 1:                                          /* ASCII bitmap         */
        for (i = 0; i < total; i++) {
            int c = n_sig();                         /* digits may abut      */
            unsigned v;
            if (c < 0) { um_set_error("truncated PNM raster"); return -1; }
            if (c != '0' && c != '1') {
                um_set_error("PBM sample is not 0 or 1");
                return -1;
            }
            v = (c == '1') ? 0 : 255;                /* 1 = black            */
            dst[i] = UM_PX(v, v, v, 255);
        }
        break;
    case 2:                                          /* ASCII graymap        */
        for (i = 0; i < total; i++) {
            long v = n_ascii();
            unsigned s;
            if (v < 0) return -1;
            s = n_scale(v);
            dst[i] = UM_PX(s, s, s, 255);
        }
        break;
    case 3:                                          /* ASCII pixmap         */
        for (i = 0; i < total; i++) {
            long r = n_ascii(), g, b;
            if (r < 0) return -1;
            g = n_ascii(); if (g < 0) return -1;
            b = n_ascii(); if (b < 0) return -1;
            dst[i] = UM_PX(n_scale(r), n_scale(g), n_scale(b), 255);
        }
        break;
    case 4: {                                        /* packed bitmap        */
        int y, x;
        for (y = 0; y < n_h; y++) {
            int acc = 0, nbits = 0;
            for (x = 0; x < n_w; x++) {
                unsigned v;
                if (nbits == 0) {
                    acc = n_byte();
                    if (acc < 0) {
                        um_set_error("truncated PNM raster");
                        return -1;
                    }
                    nbits = 8;
                }
                v = (acc & 0x80) ? 0 : 255;          /* MSB first, 1 = black */
                acc <<= 1;
                nbits--;
                dst[(long)y * n_w + x] = UM_PX(v, v, v, 255);
            }                                        /* row pad discarded    */
        }
        break;
    }
    case 5:                                          /* binary graymap       */
        for (i = 0; i < total; i++) {
            long v = n_bin();
            unsigned s;
            if (v < 0) { um_set_error("truncated PNM raster"); return -1; }
            s = n_scale(v);
            dst[i] = UM_PX(s, s, s, 255);
        }
        break;
    default:                                         /* 6: binary pixmap     */
        for (i = 0; i < total; i++) {
            long r = n_bin(), g = n_bin(), b = n_bin();
            if (r < 0 || g < 0 || b < 0) {
                um_set_error("truncated PNM raster");
                return -1;
            }
            dst[i] = UM_PX(n_scale(r), n_scale(g), n_scale(b), 255);
        }
        break;
    }

    n_done = 1;
    if (delay_ms) *delay_ms = 0;
    return 1;
}

static void pnm_rewind(void) { n_done = 0; }

static void pnm_close(void) { n_done = 0; }

const um_idecoder um_idec_pnm =
    { "PNM", pnm_probe, pnm_open, pnm_frame, pnm_rewind, pnm_close };

/* ===========================================================================
 * unomedia - JPEG (JFIF) decoder.
 *
 * Written from scratch against ITU-T T.81. Nothing here is derived from
 * libjpeg, stb_image or any other implementation; the only borrowed data is
 * the specification's own: the zigzag scan order (Figure A.6) and the DCT
 * basis cosines, which are mathematics, not authorship.
 *
 * ---- pipeline --------------------------------------------------------------
 *   markers -> DQT/DHT/DRI/SOF0-1/SOS -> per MCU row:
 *     Huffman entropy decode -> dequantise -> integer IDCT -> level shift
 *     -> (next MCU row decoded one ahead) -> triangle chroma upsample
 *     -> BT.601 full-range YCbCr->RGB -> dst rows
 *
 * The only full-image buffer is the caller's dst. Per component the decoder
 * holds two MCU-row bands (current + lookahead) plus one saved sample row,
 * because centred (bilinear "triangle") chroma upsampling needs the rows
 * above and below a band edge; the lookahead makes that exact everywhere
 * instead of approximated every 8/16 rows.
 *
 * ---- IDCT ------------------------------------------------------------------
 * Two-pass 8-point matrix IDCT in fixed point: a 14-bit .rodata table of
 * s(k)*cos((2n+1)k*pi/16), 64-bit accumulation (so hostile coefficients can
 * never overflow), and an all-AC-zero short-circuit per column/row that
 * catches the flat blocks real images are full of. Max error measured at
 * 0.48 of a grey level against the exact real-valued IDCT - well inside the
 * Annex A accuracy the 40 dB test target implies. No float anywhere.
 *
 * ---- scope (deliberate) ----------------------------------------------------
 * Decoded: baseline sequential (SOF0) and 8-bit extended sequential (SOF1),
 * Huffman coding, 1-component greyscale and 3-component YCbCr (a 3-component
 * file with ids 'R','G','B', or an Adobe APP14 transform of 0, is taken as
 * plain RGB), sampling factors 1 and 2 in either axis (4:4:4, 4:2:2, 4:4:0,
 * 4:2:0), restart markers with resync, missing-EOI tolerance.
 *
 * Recognised but refused, each with its own message (the AAC precedent -
 * "a good file this build declines" must never read as "not an image"):
 * progressive (SOF2), lossless (SOF3), hierarchical (SOF5-7/13-15),
 * arithmetic coding (SOF9-11), 12-bit precision, 4-component CMYK/YCCK,
 * sampling factors 3 and 4, multi-scan sequential.
 *
 * Every header field is distrusted: dimension guards before any allocation,
 * segment-length accounting on every marker, bounded resync loops, and a
 * bit reader that pads with zeros at EOF/marker instead of reading on - a
 * truncated scan finishes (garbage tail) or fails cleanly, never hangs.
 * ======================================================================== */
#include "unomedia.h"
#include <string.h>
#include <stdint.h>

/* ---- spec constant data --------------------------------------------------- */

/* de-zigzag: natural position of zigzag index k (T.81 Figure A.6) */
static const uint8_t kZig[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* kIdct[k][n] = round(16384 * s(k) * cos((2n+1)*k*pi/16)),
 * s(0) = 1/(2*sqrt(2)), s(k>0) = 1/2 - the 1-D IDCT basis, 14-bit fixed */
static const int16_t kIdct[8][8] = {
    { 5793,  5793,  5793,  5793,  5793,  5793,  5793,  5793 },
    { 8035,  6811,  4551,  1598, -1598, -4551, -6811, -8035 },
    { 7568,  3135, -3135, -7568, -7568, -3135,  3135,  7568 },
    { 6811, -1598, -8035, -4551,  4551,  8035,  1598, -6811 },
    { 5793, -5793, -5793,  5793,  5793, -5793, -5793,  5793 },
    { 4551, -8035,  1598,  6811, -6811, -1598,  8035, -4551 },
    { 3135, -7568,  7568, -3135, -3135,  7568, -7568,  3135 },
    { 1598, -4551,  6811, -8035,  8035, -6811,  4551, -1598 }
};

/* ---- buffered sequential reader over um_read ------------------------------ */
#define JBUF 1024

typedef struct {
    long base;                  /* absolute file offset of buf[0]            */
    int  len, idx;
    unsigned char buf[JBUF];
} jrd;

static void jr_seek(jrd *r, long off) { r->base = off; r->len = r->idx = 0; }
static long jr_tell(const jrd *r)     { return r->base + r->idx; }

static int jr_byte(jrd *r)            /* next byte, -1 at EOF / I/O error   */
{
    if (r->idx >= r->len) {
        long n;
        r->base += r->len;
        r->idx = 0; r->len = 0;
        n = um_read(r->base, r->buf, JBUF);
        if (n <= 0) return -1;
        r->len = (int)n;
    }
    return r->buf[r->idx++];
}

static int jr_u16(jrd *r)             /* big-endian 16-bit, -1 at EOF       */
{
    int a = jr_byte(r), b;
    if (a < 0) return -1;
    b = jr_byte(r);
    if (b < 0) return -1;
    return (a << 8) | b;
}

static void jr_skip(jrd *r, long n)
{
    long avail = r->len - r->idx;
    if (n <= avail) r->idx += (int)n;
    else            jr_seek(r, r->base + r->idx + n);
}

/* ---- Huffman tables (T.81 Annex C build, Annex F decode) ------------------ */
typedef struct {
    uint16_t fast[256];         /* (len<<8)|symbol for codes <= 8 bits       */
    int32_t  mincode[17], maxcode[17];
    int16_t  valptr[17];
    uint8_t  vals[256];
    int      defined;
} jhuff;

static int jh_build(jhuff *h, const uint8_t *counts, const uint8_t *vals,
                    int nvals)
{
    uint32_t code = 0;
    int l, i, k = 0;

    memset(h->fast, 0, sizeof h->fast);
    memcpy(h->vals, vals, (size_t)nvals);
    for (l = 1; l <= 16; l++) {
        h->valptr[l]  = (int16_t)k;
        h->mincode[l] = (int32_t)code;
        if (counts[l - 1]) {
            code += counts[l - 1];
            k    += counts[l - 1];
            if (code > (1u << l)) return 0;       /* over-subscribed tree    */
            h->maxcode[l] = (int32_t)code - 1;
            if (l <= 8) {                         /* short codes: direct LUT */
                for (i = 0; i < counts[l - 1]; i++) {
                    uint32_t c0  = (uint32_t)(h->mincode[l] + i) << (8 - l);
                    int      rep = 1 << (8 - l), j;
                    uint16_t e   = (uint16_t)((l << 8) |
                                              h->vals[h->valptr[l] + i]);
                    for (j = 0; j < rep; j++)
                        h->fast[c0 + (uint32_t)j] = e;
                }
            }
        } else {
            h->maxcode[l] = -1;
        }
        code <<= 1;
    }
    h->defined = 1;
    return 1;
}

/* ---- decoder state -------------------------------------------------------- */
typedef struct {
    int      id, h, v, tq;      /* from SOF                                  */
    int      td, ta;            /* Huffman table ids from SOS                */
    int32_t  dcpred;
    int      cw;                /* padded band width in component samples    */
    int      cwv, chv;          /* valid component width / total valid rows  */
    uint8_t *band[2];           /* cw * (v*8) each: current + lookahead      */
    uint8_t *prev;              /* last sample row of the previous band      */
} jcomp;

typedef struct {
    jrd      rd;
    /* entropy bit reader */
    uint32_t bb;                /* bit buffer, MSB-first, bc valid bits      */
    int      bc;
    int      marker;            /* pending marker byte seen by the filler   */
    int      starved;           /* EOF reached: pad with zero bits           */
    /* tables */
    uint16_t qt[4][64];         /* zigzag order, as stored in DQT            */
    int      qt_ok[4];
    jhuff    hf[8];             /* 0-3 DC, 4-7 AC                            */
    /* frame */
    int      w, h, ncomp;
    jcomp    comp[3];
    int      hmax, vmax;
    int      mcux, mcuy;
    int      ri;                /* restart interval in MCUs (0 = none)       */
    int      rgb;               /* 3 components already RGB, no transform    */
    int      adobe, adobe_tf;
    int      scanord[3];        /* component index in SOS (= MCU) order      */
    long     scan_pos;          /* file offset of the entropy-coded data     */
    long     mcu_count;
    int      done;
    uint8_t *bigbuf;            /* one allocation carved into bands/prev/tmp */
    uint8_t *tmp[3];            /* one full-width upsampled row per comp     */
} jstate;

static jstate *g;               /* the decoder's entire .bss                 */

/* ---- entropy bit reader ---------------------------------------------------
 * Fills MSB-first. 0xFF 0x00 unstuffs to a data 0xFF; any other marker is
 * latched (never read past) and the reader pads with zero bits from then
 * on, as it does at EOF - decode loops are bounded by MCU count, so a
 * truncated stream ends, it never spins. */
static void jb_init(jstate *s, long off)
{
    jr_seek(&s->rd, off);
    s->bb = 0; s->bc = 0; s->marker = 0; s->starved = 0;
}

static void jb_fill(jstate *s)
{
    while (s->bc <= 24) {
        int b = 0;
        if (!s->marker && !s->starved) {
            b = jr_byte(&s->rd);
            if (b < 0) { s->starved = 1; b = 0; }
            else if (b == 0xFF) {
                int c;
                do c = jr_byte(&s->rd); while (c == 0xFF);
                if (c < 0)       { s->starved = 1; b = 0; }
                else if (c == 0) b = 0xFF;              /* stuffed data byte */
                else             { s->marker = c; b = 0; }
            }
        }
        s->bb = (s->bb << 8) | (uint32_t)b;
        s->bc += 8;
    }
}

static uint32_t jb_peek(jstate *s, int n)     /* 1 <= n <= 16 */
{
    jb_fill(s);
    return (s->bb >> (s->bc - n)) & ((1u << n) - 1u);
}

static int32_t jb_get(jstate *s, int n)       /* 0 <= n <= 16 */
{
    uint32_t v;
    if (n == 0) return 0;
    v = jb_peek(s, n);
    s->bc -= n;
    return (int32_t)v;
}

/* T.81 F.2.2.1 EXTEND: low-order bits -> signed coefficient value */
static int32_t jx_extend(int32_t v, int t)
{
    if (t == 0) return 0;
    if (v < (1 << (t - 1))) v -= (1 << t) - 1;
    return v;
}

/* T.81 F.2.2.3 DECODE: fast 8-bit LUT, then the maxcode walk for 9..16 */
static int jh_dec(jstate *s, jhuff *h)
{
    uint32_t v16 = jb_peek(s, 16);
    uint16_t e   = h->fast[v16 >> 8];
    int l;

    if (e) { s->bc -= e >> 8; return e & 0xFF; }
    for (l = 9; l <= 16; l++) {
        int32_t c = (int32_t)(v16 >> (16 - l));
        if (h->maxcode[l] >= 0 && c >= h->mincode[l] && c <= h->maxcode[l]) {
            s->bc -= l;
            return h->vals[h->valptr[l] + (c - h->mincode[l])];
        }
    }
    return -1;
}

/* ---- IDCT ----------------------------------------------------------------- */
static void jd_idct(const int32_t *blk, uint8_t *dst, int stride)
{
    int32_t p[64];
    int r, c, k;

    /* pass 1: columns; result keeps 3 fractional bits (14-bit table >> 11) */
    for (c = 0; c < 8; c++) {
        if (!(blk[8 + c] | blk[16 + c] | blk[24 + c] | blk[32 + c] |
              blk[40 + c] | blk[48 + c] | blk[56 + c])) {
            int32_t v = (int32_t)
                (((int64_t)blk[c] * kIdct[0][0] + (1 << 10)) >> 11);
            for (r = 0; r < 8; r++) p[r * 8 + c] = v;
            continue;
        }
        for (r = 0; r < 8; r++) {
            int64_t acc = 0;
            for (k = 0; k < 8; k++)
                acc += (int64_t)blk[k * 8 + c] * kIdct[k][r];
            p[r * 8 + c] = (int32_t)((acc + (1 << 10)) >> 11);
        }
    }
    /* pass 2: rows; >>17 removes 14+3 scale bits, +128 level shift, clamp */
    for (r = 0; r < 8; r++) {
        uint8_t       *o  = dst + (long)r * stride;
        const int32_t *pr = p + r * 8;
        if (!(pr[1] | pr[2] | pr[3] | pr[4] | pr[5] | pr[6] | pr[7])) {
            int32_t v = (int32_t)
                (((int64_t)pr[0] * kIdct[0][0] +
                  ((int64_t)128 << 17) + (1 << 16)) >> 17);
            uint8_t b = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            memset(o, b, 8);
            continue;
        }
        for (c = 0; c < 8; c++) {
            int64_t acc = 0;
            int32_t v;
            for (k = 0; k < 8; k++)
                acc += (int64_t)pr[k] * kIdct[k][c];
            v = (int32_t)((acc + ((int64_t)128 << 17) + (1 << 16)) >> 17);
            o[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}

/* ---- one 8x8 data unit ---------------------------------------------------- */
static int jd_block(jstate *s, jcomp *cp, uint8_t *dst, int stride)
{
    int32_t         blk[64];
    const uint16_t *q = s->qt[cp->tq];
    int             k, t, rs;

    memset(blk, 0, sizeof blk);

    t = jh_dec(s, &s->hf[cp->td]);
    if (t < 0 || t > 16) {
        um_set_error("JPEG: bad Huffman code in scan");
        return 0;
    }
    cp->dcpred += jx_extend(jb_get(s, t), t);
    if (cp->dcpred >  32767) cp->dcpred =  32767;   /* hostile-stream guard: */
    if (cp->dcpred < -32768) cp->dcpred = -32768;   /* keeps dequant in i32  */
    blk[0] = cp->dcpred * (int32_t)q[0];

    k = 1;
    while (k < 64) {
        int r, sz;
        rs = jh_dec(s, &s->hf[4 + cp->ta]);
        if (rs < 0) {
            um_set_error("JPEG: bad Huffman code in scan");
            return 0;
        }
        r = rs >> 4; sz = rs & 15;
        if (sz == 0) {
            if (r != 15) break;                      /* EOB                  */
            k += 16;                                 /* ZRL                  */
        } else {
            k += r;
            if (k > 63) {
                um_set_error("JPEG: AC coefficient run past end of block");
                return 0;
            }
            blk[kZig[k]] = jx_extend(jb_get(s, sz), sz) * (int32_t)q[k];
            k++;
        }
    }
    jd_idct(blk, dst, stride);
    return 1;
}

/* ---- restart markers ------------------------------------------------------ */
static int jb_restart(jstate *s)
{
    int j;

    s->bb = 0; s->bc = 0;            /* drop pad bits (and any phantom fill) */
    if (!s->marker) {
        /* the filler has not reached the marker yet: resync byte-by-byte */
        int b;
        for (;;) {
            b = jr_byte(&s->rd);
            if (b < 0) { um_set_error("JPEG: truncated at restart"); return 0; }
            if (b != 0xFF) continue;
            do b = jr_byte(&s->rd); while (b == 0xFF);
            if (b < 0) { um_set_error("JPEG: truncated at restart"); return 0; }
            if (b == 0) continue;                    /* stuffed byte, go on  */
            s->marker = b;
            break;
        }
    }
    if (s->marker >= 0xD0 && s->marker <= 0xD7) {    /* any RSTn resyncs     */
        s->marker = 0;
        for (j = 0; j < s->ncomp; j++) s->comp[j].dcpred = 0;
        return 1;
    }
    um_set_error("JPEG: missing restart marker");
    return 0;
}

/* ---- one MCU row into a band slot ----------------------------------------- */
static int jd_band(jstate *s, int slot)
{
    int mx, j, by, bx;

    for (mx = 0; mx < s->mcux; mx++) {
        if (s->ri && s->mcu_count && s->mcu_count % s->ri == 0)
            if (!jb_restart(s)) return 0;
        for (j = 0; j < s->ncomp; j++) {
            jcomp *cp = &s->comp[s->scanord[j]];
            for (by = 0; by < cp->v; by++)
                for (bx = 0; bx < cp->h; bx++) {
                    uint8_t *d = cp->band[slot] +
                                 (long)by * 8 * cp->cw +
                                 ((long)mx * cp->h + bx) * 8;
                    if (!jd_block(s, cp, d, cp->cw)) return 0;
                }
        }
        s->mcu_count++;
    }
    return 1;
}

/* ---- upsampling + colour --------------------------------------------------
 * Locate component sample row `row` (global, clamped by the caller) while
 * band `band` sits in slot `slot`: it is either in the current band, the
 * saved last row of the previous band, or row 0 of the lookahead band. */
static const uint8_t *jp_row(const jcomp *cp, int band, int slot, long row)
{
    long lo = (long)band * cp->v * 8;

    if (row < 0)            row = 0;
    if (row > cp->chv - 1)  row = cp->chv - 1;
    if (row < lo)                    return cp->prev;
    if (row < lo + cp->v * 8)
        return cp->band[slot] + (row - lo) * cp->cw;
    return cp->band[slot ^ 1];                       /* lookahead, row 0     */
}

/* Build component ci's contribution to output row y into s->tmp[ci], at
 * full image width. Factor-2 axes use the centred triangle filter
 * (3*near + far, the JFIF-suggested reconstruction); factor-1 axes copy. */
static void jp_build_row(jstate *s, int ci, int band, int slot, int y)
{
    jcomp         *cp = &s->comp[ci];
    uint8_t       *o  = s->tmp[ci];
    int            fv = s->vmax / cp->v, fh = s->hmax / cp->h;
    const uint8_t *pa, *pb;
    long           c, cn;
    int            x;

    if (fv == 2) { c = y >> 1; cn = (y & 1) ? c + 1 : c - 1; }
    else         { c = y;      cn = c; }
    pa = jp_row(cp, band, slot, c);
    pb = jp_row(cp, band, slot, cn);

    if (fh == 1 && fv == 1) { memcpy(o, pa, (size_t)s->w); return; }

    if (fh == 2) {
        int lim = cp->cwv - 1;
        for (x = 0; x < s->w; x++) {
            int cx = x >> 1;
            int cx2 = (x & 1) ? cx + 1 : cx - 1;
            int va, vb;
            if (cx2 < 0) cx2 = 0; else if (cx2 > lim) cx2 = lim;
            va = 3 * pa[cx]  + pb[cx];               /* vertical, scale 4    */
            vb = 3 * pa[cx2] + pb[cx2];
            o[x] = (uint8_t)((3 * va + vb + 8) >> 4);/* horizontal, scale 16 */
        }
    } else {                                         /* fh == 1, fv == 2     */
        for (x = 0; x < s->w; x++)
            o[x] = (uint8_t)((3 * pa[x] + pb[x] + 2) >> 2);
    }
}

static int ju_clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void jp_emit(jstate *s, int band, um_px *dst)
{
    int slot = band & 1;
    int y0   = band * s->vmax * 8;
    int y1   = y0 + s->vmax * 8;
    int y, x;

    if (y1 > s->h) y1 = s->h;
    for (y = y0; y < y1; y++) {
        um_px *op = dst + (long)y * s->w;
        if (s->ncomp == 1) {
            jp_build_row(s, 0, band, slot, y);
            for (x = 0; x < s->w; x++) {
                unsigned v = s->tmp[0][x];
                op[x] = UM_PX(v, v, v, 0xFF);
            }
        } else {
            jp_build_row(s, 0, band, slot, y);
            jp_build_row(s, 1, band, slot, y);
            jp_build_row(s, 2, band, slot, y);
            if (s->rgb) {
                for (x = 0; x < s->w; x++)
                    op[x] = UM_PX(s->tmp[0][x], s->tmp[1][x],
                                  s->tmp[2][x], 0xFF);
            } else {
                /* BT.601 full range (JFIF), 16.16 fixed point */
                for (x = 0; x < s->w; x++) {
                    int yv = s->tmp[0][x];
                    int cb = s->tmp[1][x] - 128;
                    int cr = s->tmp[2][x] - 128;
                    int r  = yv + (( 91881 * cr + 32768) >> 16);
                    int gr = yv - (( 22554 * cb + 46802 * cr + 32768) >> 16);
                    int b  = yv + ((116130 * cb + 32768) >> 16);
                    op[x] = UM_PX(ju_clamp(r), ju_clamp(gr),
                                  ju_clamp(b), 0xFF);
                }
            }
        }
    }
}

/* ---- header parsing ------------------------------------------------------- */

/* next marker byte: scan to 0xFF (resync-tolerant), swallow fill 0xFFs */
static int jm_next(jstate *s)
{
    int b;
    do { b = jr_byte(&s->rd); if (b < 0) return -1; } while (b != 0xFF);
    do b = jr_byte(&s->rd); while (b == 0xFF);
    return b;                                        /* -1 on EOF            */
}

static int jp_parse_dqt(jstate *s, long seg)
{
    while (seg > 0) {
        int pt = jr_byte(&s->rd), pq, tq, i;
        if (pt < 0) { um_set_error("JPEG: truncated header"); return 0; }
        pq = pt >> 4; tq = pt & 15;
        if (pq > 1 || tq > 3) {
            um_set_error("JPEG: bad quantisation table header");
            return 0;
        }
        if (seg < 1 + 64L * (pq + 1)) {
            um_set_error("JPEG: truncated quantisation table");
            return 0;
        }
        for (i = 0; i < 64; i++) {
            int v = pq ? jr_u16(&s->rd) : jr_byte(&s->rd);
            if (v < 0) { um_set_error("JPEG: truncated header"); return 0; }
            s->qt[tq][i] = (uint16_t)v;
        }
        s->qt_ok[tq] = 1;
        seg -= 1 + 64L * (pq + 1);
    }
    return 1;
}

static int jp_parse_dht(jstate *s, long seg)
{
    uint8_t counts[16], vals[256];
    while (seg >= 17) {
        int tcth = jr_byte(&s->rd), tc, th, i;
        long sum = 0;
        if (tcth < 0) { um_set_error("JPEG: truncated header"); return 0; }
        tc = tcth >> 4; th = tcth & 15;
        if (tc > 1 || th > 3) {
            um_set_error("JPEG: bad Huffman table header");
            return 0;
        }
        for (i = 0; i < 16; i++) {
            int v = jr_byte(&s->rd);
            if (v < 0) { um_set_error("JPEG: truncated header"); return 0; }
            counts[i] = (uint8_t)v;
            sum += v;
        }
        if (sum > 256 || seg < 17 + sum) {
            um_set_error("JPEG: bad Huffman table");
            return 0;
        }
        for (i = 0; i < sum; i++) {
            int v = jr_byte(&s->rd);
            if (v < 0) { um_set_error("JPEG: truncated header"); return 0; }
            vals[i] = (uint8_t)v;
        }
        if (!jh_build(&s->hf[tc * 4 + th], counts, vals, (int)sum)) {
            um_set_error("JPEG: bad Huffman table");
            return 0;
        }
        seg -= 17 + sum;
    }
    if (seg > 0) jr_skip(&s->rd, seg);
    return 1;
}

static int jp_parse_sof(jstate *s, long seg)
{
    int p, wd, ht, nf, j;

    if (s->w) { um_set_error("JPEG: multiple frame headers"); return 0; }
    if (seg < 6) { um_set_error("JPEG: truncated frame header"); return 0; }
    p  = jr_byte(&s->rd);
    ht = jr_u16(&s->rd);
    wd = jr_u16(&s->rd);
    nf = jr_byte(&s->rd);
    if (p < 0 || ht < 0 || wd < 0 || nf < 0) {
        um_set_error("JPEG: truncated header");
        return 0;
    }
    if (p != 8) {
        um_set_error(p == 12 ? "12-bit JPEG - not decoded in this build"
                             : "JPEG: unsupported sample precision");
        return 0;
    }
    if (nf == 4) {
        um_set_error("4-component (CMYK/YCCK) JPEG - not decoded in this build");
        return 0;
    }
    if (nf != 1 && nf != 3) {
        um_set_error("JPEG: unsupported component count");
        return 0;
    }
    if (wd < 1 || ht < 1 || wd > UM_MAX_DIM || ht > UM_MAX_DIM ||
        (long)wd * ht > UM_MAX_PIXELS) {
        um_set_error("JPEG: image dimensions out of range");
        return 0;
    }
    if (seg < 6 + 3L * nf) {
        um_set_error("JPEG: truncated frame header");
        return 0;
    }
    for (j = 0; j < nf; j++) {
        jcomp *cp = &s->comp[j];
        int id = jr_byte(&s->rd);
        int hv = jr_byte(&s->rd);
        int tq = jr_byte(&s->rd);
        if (id < 0 || hv < 0 || tq < 0) {
            um_set_error("JPEG: truncated header");
            return 0;
        }
        cp->id = id;
        cp->h  = hv >> 4;
        cp->v  = hv & 15;
        cp->tq = tq;
        if (cp->h == 3 || cp->h == 4 || cp->v == 3 || cp->v == 4) {
            um_set_error(
                "JPEG sampling factor 3 or 4 - not decoded in this build");
            return 0;
        }
        if (cp->h < 1 || cp->h > 2 || cp->v < 1 || cp->v > 2) {
            um_set_error("JPEG: bad sampling factor");
            return 0;
        }
        if (tq > 3) {
            um_set_error("JPEG: bad quantisation table selector");
            return 0;
        }
    }
    jr_skip(&s->rd, seg - (6 + 3L * nf));
    s->w = wd; s->h = ht; s->ncomp = nf;
    return 1;
}

static int jp_parse_sos(jstate *s, long seg)
{
    int ns, j, k, used[3] = { 0, 0, 0 };
    int ss, se, a;

    if (!s->w) { um_set_error("JPEG: scan before frame header"); return 0; }
    ns = jr_byte(&s->rd);
    if (ns < 0) { um_set_error("JPEG: truncated header"); return 0; }
    if (ns != s->ncomp) {
        um_set_error(s->ncomp > 1 && ns >= 1 && ns < s->ncomp
                     ? "multi-scan sequential JPEG - not decoded in this build"
                     : "JPEG: bad scan component count");
        return 0;
    }
    if (seg < 1 + 2L * ns + 3) {
        um_set_error("JPEG: truncated scan header");
        return 0;
    }
    for (j = 0; j < ns; j++) {
        int cs = jr_byte(&s->rd);
        int tt = jr_byte(&s->rd);
        if (cs < 0 || tt < 0) {
            um_set_error("JPEG: truncated header");
            return 0;
        }
        for (k = 0; k < s->ncomp; k++)
            if (s->comp[k].id == cs && !used[k]) break;
        if (k == s->ncomp) {
            um_set_error("JPEG: scan references unknown component");
            return 0;
        }
        used[k] = 1;
        s->scanord[j] = k;
        s->comp[k].td = tt >> 4;
        s->comp[k].ta = tt & 15;
        if (s->comp[k].td > 3 || s->comp[k].ta > 3) {
            um_set_error("JPEG: bad Huffman table selector");
            return 0;
        }
    }
    ss = jr_byte(&s->rd);
    se = jr_byte(&s->rd);
    a  = jr_byte(&s->rd);
    if (ss < 0 || se < 0 || a < 0) {
        um_set_error("JPEG: truncated header");
        return 0;
    }
    if (ss != 0 || se != 63 || a != 0) {
        um_set_error("JPEG: unexpected scan parameters");
        return 0;
    }
    for (j = 0; j < s->ncomp; j++) {
        jcomp *cp = &s->comp[j];
        if (!s->qt_ok[cp->tq]) {
            um_set_error("JPEG: missing quantisation table");
            return 0;
        }
        if (!s->hf[cp->td].defined || !s->hf[4 + cp->ta].defined) {
            um_set_error("JPEG: missing Huffman table");
            return 0;
        }
    }
    return 1;
}

/* ---- the vtable ----------------------------------------------------------- */
static void jp_close(void)
{
    if (!g) return;
    um_free(g->bigbuf);
    um_free(g);
    g = 0;
}

static int jp_probe(const unsigned char *h, long n, const char *ext)
{
    if (n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return 1;
    if (n >= 2 && h[0] == 0xFF && h[1] == 0xD8 &&
        (!strcmp(ext, "JPG") || !strcmp(ext, "JPEG") || !strcmp(ext, "JFIF")))
        return 1;
    return 0;
}

static int jp_open(um_image_info *info)
{
    jstate       *s;
    unsigned long total;
    uint8_t      *cut;
    int           j;

    jp_close();                                      /* defensive            */
    s = (jstate *)um_alloc(sizeof *s);
    if (!s) { um_set_error("JPEG: out of memory"); return 0; }
    memset(s, 0, sizeof *s);
    g = s;                                           /* jp_close now frees   */

    jr_seek(&s->rd, 0);
    if (jr_byte(&s->rd) != 0xFF || jr_byte(&s->rd) != 0xD8) {
        um_set_error("JPEG: missing SOI marker");
        goto fail;
    }

    for (;;) {
        int  m = jm_next(s);
        long seg;
        if (m < 0)   { um_set_error("JPEG: truncated header"); goto fail; }
        if (m == 0xD8) continue;                     /* stray SOI            */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;   /* no length  */
        if (m == 0xD9) { um_set_error("JPEG: no image data"); goto fail; }
        if (m == 0x00) { um_set_error("JPEG: bad marker"); goto fail; }

        seg = jr_u16(&s->rd);
        if (seg < 2) { um_set_error("JPEG: bad segment length"); goto fail; }
        seg -= 2;

        switch (m) {
        case 0xC0: case 0xC1:                        /* SOF0 / SOF1          */
            if (!jp_parse_sof(s, seg)) goto fail;
            break;
        case 0xC2:
            um_set_error("progressive JPEG - not decoded in this build");
            goto fail;
        case 0xC3:
            um_set_error("lossless JPEG - not decoded in this build");
            goto fail;
        case 0xC5: case 0xC6: case 0xC7:
        case 0xCD: case 0xCE: case 0xCF:
            um_set_error("hierarchical JPEG - not decoded in this build");
            goto fail;
        case 0xC9: case 0xCA: case 0xCB:
            um_set_error("arithmetic-coded JPEG - not decoded in this build");
            goto fail;
        case 0xC4:                                   /* DHT                  */
            if (!jp_parse_dht(s, seg)) goto fail;
            break;
        case 0xDB:                                   /* DQT                  */
            if (!jp_parse_dqt(s, seg)) goto fail;
            break;
        case 0xDD:                                   /* DRI                  */
            if (seg < 2) { um_set_error("JPEG: bad DRI"); goto fail; }
            s->ri = jr_u16(&s->rd);
            if (s->ri < 0) { um_set_error("JPEG: truncated header"); goto fail; }
            jr_skip(&s->rd, seg - 2);
            break;
        case 0xEE:                                   /* APP14 (Adobe)        */
            if (seg >= 12) {
                unsigned char ad[12];
                int i, v;
                for (i = 0; i < 12; i++) {
                    v = jr_byte(&s->rd);
                    if (v < 0) {
                        um_set_error("JPEG: truncated header");
                        goto fail;
                    }
                    ad[i] = (unsigned char)v;
                }
                if (!memcmp(ad, "Adobe", 5)) {
                    s->adobe    = 1;
                    s->adobe_tf = ad[11];
                }
                jr_skip(&s->rd, seg - 12);
            } else {
                jr_skip(&s->rd, seg);
            }
            break;
        case 0xDA:                                   /* SOS                  */
            if (!jp_parse_sos(s, seg)) goto fail;
            goto scan_ready;
        default:                                     /* APPn / COM / rest    */
            jr_skip(&s->rd, seg);
            break;
        }
    }

scan_ready:
    /* single-component scans are non-interleaved: sampling is moot (A.2.2) */
    if (s->ncomp == 1) { s->comp[0].h = 1; s->comp[0].v = 1; }
    s->hmax = 1; s->vmax = 1;
    for (j = 0; j < s->ncomp; j++) {
        if (s->comp[j].h > s->hmax) s->hmax = s->comp[j].h;
        if (s->comp[j].v > s->vmax) s->vmax = s->comp[j].v;
    }
    s->mcux = (s->w + 8 * s->hmax - 1) / (8 * s->hmax);
    s->mcuy = (s->h + 8 * s->vmax - 1) / (8 * s->vmax);

    s->rgb = (s->ncomp == 3 &&
              ((s->comp[0].id == 'R' && s->comp[1].id == 'G' &&
                s->comp[2].id == 'B') ||
               (s->adobe && s->adobe_tf == 0)));

    total = 0;
    for (j = 0; j < s->ncomp; j++) {
        jcomp *cp = &s->comp[j];
        cp->cw  = s->mcux * cp->h * 8;
        cp->cwv = (s->w * cp->h + s->hmax - 1) / s->hmax;
        cp->chv = (int)(((long)s->h * cp->v + s->vmax - 1) / s->vmax);
        total  += 2ul * (unsigned long)cp->cw * (unsigned long)(cp->v * 8)
                  + (unsigned long)cp->cw + (unsigned long)s->w;
    }
    s->bigbuf = (uint8_t *)um_alloc(total);
    if (!s->bigbuf) { um_set_error("JPEG: out of memory"); goto fail; }
    cut = s->bigbuf;
    for (j = 0; j < s->ncomp; j++) {
        jcomp *cp = &s->comp[j];
        long   bandsz = (long)cp->cw * (cp->v * 8);
        cp->band[0] = cut; cut += bandsz;
        cp->band[1] = cut; cut += bandsz;
        cp->prev    = cut; cut += cp->cw;
        s->tmp[j]   = cut; cut += s->w;
    }

    s->scan_pos = jr_tell(&s->rd);
    info->format = "JPEG";
    info->w      = s->w;
    info->h      = s->h;
    info->bpp    = s->ncomp == 1 ? 8 : 24;
    info->alpha  = 0;
    info->frames = 1;
    return 1;

fail:
    jp_close();
    return 0;
}

static int jp_frame(um_px *dst, int *delay_ms)
{
    jstate *s = g;
    int     i, j;

    if (!s) { um_set_error("JPEG: no open image"); return -1; }
    if (s->done) return 0;

    jb_init(s, s->scan_pos);
    s->mcu_count = 0;
    for (j = 0; j < s->ncomp; j++) s->comp[j].dcpred = 0;

    if (!jd_band(s, 0)) return -1;                   /* band 0 -> slot 0     */
    for (j = 0; j < s->ncomp; j++)                   /* top edge replicates  */
        memcpy(s->comp[j].prev, s->comp[j].band[0], (size_t)s->comp[j].cw);

    for (i = 0; i < s->mcuy; i++) {
        if (i + 1 < s->mcuy && !jd_band(s, (i + 1) & 1)) return -1;
        jp_emit(s, i, dst);
        for (j = 0; j < s->ncomp; j++) {             /* save the seam row    */
            jcomp *cp = &s->comp[j];
            memcpy(cp->prev,
                   cp->band[i & 1] + (long)(cp->v * 8 - 1) * cp->cw,
                   (size_t)cp->cw);
        }
    }
    s->done = 1;
    if (delay_ms) *delay_ms = 0;
    return 1;
}

static void jp_rewind(void)
{
    if (g) g->done = 0;
}

const um_idecoder um_idec_jpg =
    { "jpg", jp_probe, jp_open, jp_frame, jp_rewind, jp_close };

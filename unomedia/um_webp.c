/* ===========================================================================
 * unomedia - the WebP decoder, written from scratch against RFC 9649 (the
 * WebP container + lossless bitstream specification). No libwebp code; the
 * normative constant tables (code-length-code order, the 120-entry LZ77
 * distance map) are specification data.
 *
 * Container: simple lossy ("VP8 "), simple lossless ("VP8L") and the
 * extended layout ("VP8X" + optional ALPH/ANIM/ANMF; ICCP/EXIF/XMP and
 * unknown chunks are skipped by their padded size). The lossy VP8 payload
 * is handed as one whole chunk to the VP8 core behind unomedia_int.h
 * (um_vp8_dims / um_vp8_decode); everything else decodes here.
 *
 * VP8L is complete: the four transforms (predictor with all 14 modes,
 * cross-color, subtract-green, color-indexing incl. pixel bundling), meta
 * prefix groups over an entropy image, the 5-code prefix ensemble per
 * group, code-length codes with repeats, LZ77 with the near-pixel distance
 * mapping, and the color cache. VP8L prefix coding is NOT deflate - codes
 * are canonical (assigned by length, then symbol value) and transmitted
 * MSB-of-code-first inside the LSB-first byte stream, so decode walks the
 * canonical count/offset tables one bit at a time.
 *
 * ALPH chunks (alpha for lossy frames): raw or VP8L-compressed (a
 * headerless image-stream whose green channel is the alpha plane), then
 * the inverse of filters 0-3 (none/horizontal/vertical/gradient).
 *
 * Animation mirrors um_gif.c: a cheap chunk census on open makes
 * info.frames exact before any pixel work, frame() composites each ANMF
 * frame into the caller's PERSISTENT canvas (cleared to transparent black
 * before frame 0), the previous frame's dispose-to-background zero-fills
 * its rect just before the next frame draws (the libwebp/-coalesce
 * convention: transparent black, the ANIM background color is a hint), and
 * blend-method 0 alpha-blends with the spec's straight-alpha formula.
 * rewind() restarts at frame 0; looping is the caller's call.
 *
 * All buffers come from um_alloc and are freed by close() or before the
 * failing return; header dimensions are guarded before any allocation.
 * ======================================================================== */
#include "unomedia.h"
#include "unomedia_int.h"
#include <string.h>
#include <stdint.h>

static uint32_t rd24le(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }
static uint32_t rd32le(const unsigned char *p)
{ return rd24le(p) | ((uint32_t)p[3] << 24); }

/* exact read of n bytes at off (possibly through short reads) */
static int rdn(long off, unsigned char *dst, long n)
{
    while (n > 0) {
        long got = um_read(off, dst, n);
        if (got <= 0) return 0;
        off += got; dst += got; n -= got;
    }
    return 1;
}

/* ===========================================================================
 * VP8L bit reader: bytes in stream order, bits LSB-first. Reading past the
 * end of the byte range feeds zeros and latches `ovf` - checked after each
 * complete image-data section, so truncation fails precisely instead of
 * decoding zero-noise to completion.
 * ======================================================================== */
typedef struct {
    long next, end;             /* next fetch offset, one past last byte  */
    uint32_t bits;
    int  nbits;
    int  ovf;
    int  bpos, blen;
    unsigned char buf[512];
} vbr;

static void vbr_init(vbr *b, long off, long len)
{
    b->next = off; b->end = off + len;
    b->bits = 0; b->nbits = 0; b->ovf = 0; b->bpos = 0; b->blen = 0;
}

static int vbr_byte(vbr *b)
{
    if (b->bpos >= b->blen) {
        long want = b->end - b->next;
        if (want > (long)sizeof b->buf) want = (long)sizeof b->buf;
        if (want <= 0 || um_read(b->next, b->buf, want) != want) {
            b->ovf = 1;
            return 0;
        }
        b->next += want; b->blen = (int)want; b->bpos = 0;
    }
    return b->buf[b->bpos++];
}

static uint32_t vbr_bits(vbr *b, int n)         /* n in [0..18] */
{
    uint32_t v;
    while (b->nbits < n) {
        b->bits |= (uint32_t)vbr_byte(b) << b->nbits;
        b->nbits += 8;
    }
    v = b->bits & ((n == 0) ? 0u : ((1u << n) - 1u));
    b->bits >>= n;
    b->nbits -= n;
    return v;
}

/* ===========================================================================
 * Canonical prefix codes. cnt[len] counts codes of each length 1..15;
 * sym[] lists symbols in canonical order (length-major, then symbol value).
 * cnt[0] = 1 flags the single-leaf code that consumes zero bits.
 * ======================================================================== */
typedef struct {
    uint16_t cnt[16];
    uint16_t *sym;              /* um_alloc'd, freed by hfree()           */
} hcode;

static void hfree(hcode *c)
{
    um_free(c->sym);
    c->sym = 0;
}

/* build from code lengths; 1 = ok, 0 = malformed tree, -1 = out of memory */
static int hbuild(hcode *c, const unsigned char *len, int n)
{
    int i, l, nsym = 0, off[16];
    uint32_t total = 0;

    memset(c->cnt, 0, sizeof c->cnt);
    c->sym = 0;
    for (i = 0; i < n; i++)
        if (len[i]) { c->cnt[len[i]]++; nsym++; }
    if (!nsym) return 0;                         /* empty code             */

    if (nsym == 1) {                             /* single leaf: 0 bits    */
        c->sym = (uint16_t *)um_alloc(sizeof(uint16_t));
        if (!c->sym) return -1;
        memset(c->cnt, 0, sizeof c->cnt);
        c->cnt[0] = 1;
        for (i = 0; i < n; i++)
            if (len[i]) c->sym[0] = (uint16_t)i;
        return 1;
    }
    for (l = 1; l <= 15; l++) {
        total += (uint32_t)c->cnt[l] << (15 - l);
        if (total > (1u << 15)) return 0;        /* over-subscribed        */
    }
    if (total != (1u << 15)) return 0;           /* incomplete             */

    off[1] = 0;
    for (l = 2; l <= 15; l++) off[l] = off[l - 1] + c->cnt[l - 1];
    c->sym = (uint16_t *)um_alloc((unsigned long)nsym * sizeof(uint16_t));
    if (!c->sym) return -1;
    for (i = 0; i < n; i++)
        if (len[i]) c->sym[off[len[i]]++] = (uint16_t)i;
    return 1;
}

/* decode one symbol: canonical count/offset walk, MSB of the code first */
static int hget(vbr *b, const hcode *c)
{
    int len, code = 0, first = 0, idx = 0, cnt;
    if (c->cnt[0]) return c->sym[0];
    for (len = 1; len <= 15; len++) {
        code = (code << 1) | (int)vbr_bits(b, 1);
        cnt = c->cnt[len];
        if (code - first < cnt) return c->sym[idx + code - first];
        idx += cnt;
        first = (first + cnt) << 1;
    }
    return -1;
}

/* ---- read one prefix code (simple or normal form) from the stream ------- */
static int read_hcode(vbr *b, int alphabet, hcode *c)
{
    unsigned char *len;
    int r;

    len = (unsigned char *)um_alloc((unsigned long)alphabet);
    if (!len) { um_set_error("out of memory"); return 0; }
    memset(len, 0, (unsigned long)alphabet);

    if (vbr_bits(b, 1)) {                        /* simple code            */
        int nsym = (int)vbr_bits(b, 1) + 1;
        int s = (int)vbr_bits(b, vbr_bits(b, 1) ? 8 : 1);
        if (s >= alphabet)
            { um_set_error("bad VP8L prefix code"); um_free(len); return 0; }
        len[s] = 1;
        if (nsym == 2) {
            s = (int)vbr_bits(b, 8);
            if (s >= alphabet)
                { um_set_error("bad VP8L prefix code"); um_free(len); return 0; }
            len[s] = 1;
        }
    } else {                                     /* normal code            */
        static const unsigned char order[19] =
            { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10,
              11, 12, 13, 14, 15 };
        unsigned char cll[19];
        hcode clc;
        int i, ncl = 4 + (int)vbr_bits(b, 4);
        int max_symbol, sym = 0, prev = 8;

        memset(cll, 0, sizeof cll);
        for (i = 0; i < ncl; i++) cll[order[i]] = (unsigned char)vbr_bits(b, 3);
        r = hbuild(&clc, cll, 19);
        if (r != 1) {
            um_set_error(r ? "out of memory" : "bad VP8L code-length code");
            um_free(len);
            return 0;
        }
        if (vbr_bits(b, 1)) {
            int nb = 2 + 2 * (int)vbr_bits(b, 3);
            max_symbol = 2 + (int)vbr_bits(b, nb);
            if (max_symbol > alphabet) {
                um_set_error("bad VP8L prefix code");
                hfree(&clc); um_free(len);
                return 0;
            }
        } else {
            max_symbol = alphabet;
        }
        while (sym < alphabet) {
            int cl;
            if (max_symbol-- == 0) break;
            cl = hget(b, &clc);
            if (cl < 0)
                { um_set_error("bad VP8L prefix code"); hfree(&clc); um_free(len); return 0; }
            if (cl < 16) {
                len[sym++] = (unsigned char)cl;
                if (cl) prev = cl;
            } else {
                int rep, v = 0;
                if (cl == 16)      { rep = 3 + (int)vbr_bits(b, 2); v = prev; }
                else if (cl == 17)   rep = 3 + (int)vbr_bits(b, 3);
                else                 rep = 11 + (int)vbr_bits(b, 7);
                if (sym + rep > alphabet)
                    { um_set_error("bad VP8L prefix code"); hfree(&clc); um_free(len); return 0; }
                while (rep-- > 0) len[sym++] = (unsigned char)v;
            }
        }
        hfree(&clc);
        if (b->ovf)
            { um_set_error("truncated VP8L stream"); um_free(len); return 0; }
    }
    r = hbuild(c, len, alphabet);
    um_free(len);
    if (r != 1) {
        um_set_error(r ? "out of memory" : "bad VP8L prefix code");
        return 0;
    }
    return 1;
}

/* ===========================================================================
 * VP8L image data. Pixels are held as ARGB (a<<24|r<<16|g<<8|b) until the
 * final swizzle to um_px, so transform math matches the spec text 1:1.
 * ======================================================================== */
typedef struct { hcode c[5]; } hgroup;   /* green+len+cache / R / B / A / dist */

static void groups_free(hgroup *g, int n)
{
    int i, k;
    if (!g) return;
    for (i = 0; i < n; i++)
        for (k = 0; k < 5; k++)
            hfree(&g[i].c[k]);
    um_free(g);
}

/* LZ77 length/distance prefix coding: value from prefix code + extra bits */
static long prefix_val(vbr *b, int p)
{
    int eb;
    if (p < 4) return p + 1;
    eb = (p - 2) >> 1;
    return ((long)(2 + (p & 1)) << eb) + (long)vbr_bits(b, eb) + 1;
}

/* the 120 near-pixel distance codes: (xi, yi) offsets (RFC 9649 fig. 20) */
static const signed char kDistMap[120][2] = {
    {0,1},  {1,0},  {1,1},  {-1,1}, {0,2},  {2,0},  {1,2},  {-1,2},
    {2,1},  {-2,1}, {2,2},  {-2,2}, {0,3},  {3,0},  {1,3},  {-1,3},
    {3,1},  {-3,1}, {2,3},  {-2,3}, {3,2},  {-3,2}, {0,4},  {4,0},
    {1,4},  {-1,4}, {4,1},  {-4,1}, {3,3},  {-3,3}, {2,4},  {-2,4},
    {4,2},  {-4,2}, {0,5},  {3,4},  {-3,4}, {4,3},  {-4,3}, {5,0},
    {1,5},  {-1,5}, {5,1},  {-5,1}, {2,5},  {-2,5}, {5,2},  {-5,2},
    {4,4},  {-4,4}, {3,5},  {-3,5}, {5,3},  {-5,3}, {0,6},  {6,0},
    {1,6},  {-1,6}, {6,1},  {-6,1}, {2,6},  {-2,6}, {6,2},  {-6,2},
    {4,5},  {-4,5}, {5,4},  {-5,4}, {3,6},  {-3,6}, {6,3},  {-6,3},
    {0,7},  {7,0},  {1,7},  {-1,7}, {5,5},  {-5,5}, {7,1},  {-7,1},
    {4,6},  {-4,6}, {6,4},  {-6,4}, {2,7},  {-2,7}, {7,2},  {-7,2},
    {3,7},  {-3,7}, {7,3},  {-7,3}, {5,6},  {-5,6}, {6,5},  {-6,5},
    {8,0},  {4,7},  {-4,7}, {7,4},  {-7,4}, {8,1},  {8,2},  {6,6},
    {-6,6}, {8,3},  {5,7},  {-5,7}, {7,5},  {-7,5}, {8,4},  {6,7},
    {-6,7}, {7,6},  {-7,6}, {8,5},  {7,7},  {-7,7}, {8,6},  {8,7}
};

#define DIVUP(n, b) (((n) + (1 << (b)) - 1) >> (b))

/* Decode one entropy-coded image (or the spatially-coded main image when
 * meta_ok) of xs*ys pixels into out. Handles color-cache info, the
 * optional entropy image (meta prefix codes, main image only), the prefix
 * code groups and the LZ77-coded data. 1 = ok, 0 = error (um_set_error). */
static int vp8l_data(vbr *b, int xs, int ys, int meta_ok, uint32_t *out)
{
    uint32_t *cache = 0, *eimg = 0;
    hgroup *groups = 0;
    int cache_bits = 0, cache_size = 0;
    int ngroups = 1, ebits = 0, ew = 0;
    long npix = (long)xs * ys, pos = 0;
    int x = 0, y = 0, i, k, ok = 0;

    if (vbr_bits(b, 1)) {                        /* color cache info       */
        cache_bits = (int)vbr_bits(b, 4);
        if (cache_bits < 1 || cache_bits > 11)
            { um_set_error("bad VP8L color cache size"); return 0; }
        cache_size = 1 << cache_bits;
        cache = (uint32_t *)um_alloc((unsigned long)cache_size * 4);
        if (!cache) { um_set_error("out of memory"); return 0; }
        memset(cache, 0, (unsigned long)cache_size * 4);
    }

    if (meta_ok && vbr_bits(b, 1)) {             /* entropy image          */
        long en;
        int eh;
        ebits = (int)vbr_bits(b, 3) + 2;
        ew = DIVUP(xs, ebits);
        eh = DIVUP(ys, ebits);
        en = (long)ew * eh;
        eimg = (uint32_t *)um_alloc((unsigned long)en * 4);
        if (!eimg) { um_set_error("out of memory"); goto out; }
        if (!vp8l_data(b, ew, eh, 0, eimg)) goto out;
        ngroups = 0;
        for (i = 0; i < en; i++) {
            int m = (int)((eimg[i] >> 8) & 0xffff);
            if (m >= ngroups) ngroups = m + 1;
        }
    }

    groups = (hgroup *)um_alloc((unsigned long)ngroups * sizeof(hgroup));
    if (!groups) { um_set_error("out of memory"); goto out; }
    memset(groups, 0, (unsigned long)ngroups * sizeof(hgroup));
    for (i = 0; i < ngroups; i++) {
        static const int alpha_base[5] = { 256 + 24, 256, 256, 256, 40 };
        for (k = 0; k < 5; k++) {
            int a = alpha_base[k] + (k == 0 ? cache_size : 0);
            if (!read_hcode(b, a, &groups[i].c[k])) goto out;
        }
    }

    while (pos < npix) {
        hgroup *g = groups;
        int s;
        if (eimg)
            g = groups +
                ((eimg[((long)(y >> ebits)) * ew + (x >> ebits)] >> 8) & 0xffff);
        s = hget(b, &g->c[0]);
        if (s < 0) { um_set_error("bad VP8L bitstream"); goto out; }

        if (s < 256) {                           /* literal ARGB pixel     */
            int r = hget(b, &g->c[1]);
            int bl = hget(b, &g->c[2]);
            int a = hget(b, &g->c[3]);
            uint32_t px;
            if (r < 0 || bl < 0 || a < 0)
                { um_set_error("bad VP8L bitstream"); goto out; }
            px = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                 ((uint32_t)s << 8)  |  (uint32_t)bl;
            out[pos++] = px;
            if (cache)
                cache[(0x1e35a7bdu * px) >> (32 - cache_bits)] = px;
            if (++x == xs) { x = 0; y++; }
        } else if (s < 256 + 24) {               /* LZ77 backward ref      */
            long len = prefix_val(b, s - 256), dist, dc;
            int ds = hget(b, &g->c[4]);
            if (ds < 0) { um_set_error("bad VP8L bitstream"); goto out; }
            dc = prefix_val(b, ds);
            if (dc > 120) {
                dist = dc - 120;
            } else {
                dist = (long)kDistMap[dc - 1][0] +
                       (long)kDistMap[dc - 1][1] * xs;
                if (dist < 1) dist = 1;
            }
            if (dist > pos)
                { um_set_error("VP8L reference before image start"); goto out; }
            if (len > npix - pos)
                { um_set_error("VP8L copy overruns image"); goto out; }
            while (len-- > 0) {
                uint32_t px = out[pos - dist];
                out[pos++] = px;
                if (cache)
                    cache[(0x1e35a7bdu * px) >> (32 - cache_bits)] = px;
            }
            x = (int)(pos % xs);
            y = (int)(pos / xs);
        } else {                                 /* color cache hit        */
            int ci = s - (256 + 24);
            if (ci >= cache_size)                /* cache_size 0 when off  */
                { um_set_error("bad VP8L color cache index"); goto out; }
            out[pos++] = cache[ci];
            if (++x == xs) { x = 0; y++; }
        }
    }
    if (b->ovf) { um_set_error("truncated VP8L stream"); goto out; }
    ok = 1;
out:
    groups_free(groups, ngroups);
    um_free(eimg);
    um_free(cache);
    return ok;
}

/* ===========================================================================
 * VP8L transforms. Each records the image width current when it was read;
 * inverses run in reverse read order, so widths stay consistent and the
 * color-indexing inverse is the one place the width expands back.
 * ======================================================================== */
typedef struct {
    int type;                   /* 0 pred, 1 color, 2 sub-green, 3 index  */
    int bits;                   /* block bits, or width_bits for indexing */
    int xsize;                  /* image width when this transform read   */
    int dn;                     /* palette entries (indexing only)        */
    uint32_t *data;             /* sub-image / palette, um_alloc'd        */
} vtrans;

/* channel-wise wrap-around add (no carry between channels) */
static uint32_t px_add(uint32_t a, uint32_t b)
{
    return (((a & 0xff00ff00u) + (b & 0xff00ff00u)) & 0xff00ff00u) |
           (((a & 0x00ff00ffu) + (b & 0x00ff00ffu)) & 0x00ff00ffu);
}

/* channel-wise (a + b) / 2 */
static uint32_t px_avg2(uint32_t a, uint32_t b)
{
    return (a & b) + (((a ^ b) & 0xfefefefeu) >> 1);
}

static int iabs(int v) { return v < 0 ? -v : v; }

static uint32_t pred_select(uint32_t L, uint32_t T, uint32_t TL)
{
    int pa = (int)(L >> 24) + (int)(T >> 24) - (int)(TL >> 24);
    int pr = (int)((L >> 16) & 0xff) + (int)((T >> 16) & 0xff) -
             (int)((TL >> 16) & 0xff);
    int pg = (int)((L >> 8) & 0xff) + (int)((T >> 8) & 0xff) -
             (int)((TL >> 8) & 0xff);
    int pb = (int)(L & 0xff) + (int)(T & 0xff) - (int)(TL & 0xff);
    int pL = iabs(pa - (int)(L >> 24)) + iabs(pr - (int)((L >> 16) & 0xff)) +
             iabs(pg - (int)((L >> 8) & 0xff)) + iabs(pb - (int)(L & 0xff));
    int pT = iabs(pa - (int)(T >> 24)) + iabs(pr - (int)((T >> 16) & 0xff)) +
             iabs(pg - (int)((T >> 8) & 0xff)) + iabs(pb - (int)(T & 0xff));
    return (pL < pT) ? L : T;
}

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static uint32_t pred_cas_full(uint32_t L, uint32_t T, uint32_t TL)
{
    uint32_t r = 0;
    int sh;
    for (sh = 0; sh < 32; sh += 8) {
        int v = (int)((L >> sh) & 0xff) + (int)((T >> sh) & 0xff) -
                (int)((TL >> sh) & 0xff);
        r |= (uint32_t)clamp255(v) << sh;
    }
    return r;
}

static uint32_t pred_cas_half(uint32_t AV, uint32_t TL)
{
    uint32_t r = 0;
    int sh;
    for (sh = 0; sh < 32; sh += 8) {
        int a = (int)((AV >> sh) & 0xff), c = (int)((TL >> sh) & 0xff);
        r |= (uint32_t)clamp255(a + (a - c) / 2) << sh;
    }
    return r;
}

static int inv_predictor(uint32_t *img, int w, int h, const vtrans *t)
{
    int sw = DIVUP(w, t->bits);
    long i = 0;
    int x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++, i++) {
            uint32_t pred, L, T, TL, TR;
            if (y == 0) {
                pred = (x == 0) ? 0xff000000u : img[i - 1];
            } else if (x == 0) {
                pred = img[i - w];
            } else {
                int mode = (int)((t->data[((long)(y >> t->bits)) * sw +
                                          (x >> t->bits)] >> 8) & 0xff);
                L = img[i - 1];
                T = img[i - w];
                TL = img[i - w - 1];
                TR = img[i - w + 1];    /* x == w-1: (0, y), already decoded */
                switch (mode) {
                case 0:  pred = 0xff000000u; break;
                case 1:  pred = L; break;
                case 2:  pred = T; break;
                case 3:  pred = TR; break;
                case 4:  pred = TL; break;
                case 5:  pred = px_avg2(px_avg2(L, TR), T); break;
                case 6:  pred = px_avg2(L, TL); break;
                case 7:  pred = px_avg2(L, T); break;
                case 8:  pred = px_avg2(TL, T); break;
                case 9:  pred = px_avg2(T, TR); break;
                case 10: pred = px_avg2(px_avg2(L, TL), px_avg2(T, TR)); break;
                case 11: pred = pred_select(L, T, TL); break;
                case 12: pred = pred_cas_full(L, T, TL); break;
                case 13: pred = pred_cas_half(px_avg2(L, T), TL); break;
                default:
                    um_set_error("bad VP8L predictor mode");
                    return 0;
                }
            }
            img[i] = px_add(img[i], pred);
        }
    }
    return 1;
}

static int cdelta(int t, int c)     /* (int8)t * (int8)c >> 5, low 8 bits */
{
    return ((int)(signed char)t * (int)(signed char)c) >> 5;
}

static void inv_color(uint32_t *img, int w, int h, const vtrans *t)
{
    int sw = DIVUP(w, t->bits);
    long i = 0;
    int x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++, i++) {
            uint32_t cte = t->data[((long)(y >> t->bits)) * sw +
                                   (x >> t->bits)];
            int g2r = (int)(cte & 0xff);         /* blue channel of cte    */
            int g2b = (int)((cte >> 8) & 0xff);  /* green channel          */
            int r2b = (int)((cte >> 16) & 0xff); /* red channel            */
            uint32_t px = img[i];
            int g = (int)((px >> 8) & 0xff);
            int r = (int)((px >> 16) & 0xff);
            int bl = (int)(px & 0xff);
            r  = (r + cdelta(g2r, g)) & 0xff;
            bl = (bl + cdelta(g2b, g) + cdelta(r2b, r)) & 0xff;
            img[i] = (px & 0xff00ff00u) | ((uint32_t)r << 16) | (uint32_t)bl;
        }
    }
}

static void inv_subtract_green(uint32_t *img, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        uint32_t px = img[i];
        uint32_t g = (px >> 8) & 0xff;
        uint32_t r = (((px >> 16) & 0xff) + g) & 0xff;
        uint32_t bl = ((px & 0xff) + g) & 0xff;
        img[i] = (px & 0xff00ff00u) | (r << 16) | bl;
    }
}

/* expand color indices (possibly pixel-bundled) through the palette.
 * src is packed_w * h, dst is t->xsize * h; in-place when wb == 0. */
static void inv_index(const uint32_t *src, int packed_w, uint32_t *dst,
                      int h, const vtrans *t)
{
    int wb = t->bits, bpp = 8 >> wb, mask = (1 << wb) - 1;
    int x, y;
    for (y = 0; y < h; y++) {
        const uint32_t *srow = src + (long)y * packed_w;
        uint32_t *drow = dst + (long)y * t->xsize;
        for (x = 0; x < t->xsize; x++) {
            int idx = (int)((srow[x >> wb] >> 8) & 0xff);
            if (wb)
                idx = (idx >> ((x & mask) * bpp)) & ((1 << bpp) - 1);
            drow[x] = (idx < t->dn) ? t->data[idx] : 0;   /* OOB: 0 (spec) */
        }
    }
}

/* ---- full VP8L image-stream decode --------------------------------------
 * off/len bound the stream (past the 5-byte header for .webp lossless, the
 * whole byte range for ALPH). Decodes w*h pixels into dst as ARGB words;
 * the caller swizzles. 1 = ok, 0 = error. */
static int vp8l_stream(long off, long len, int w, int h, uint32_t *dst)
{
    vbr b;
    vtrans tr[4];
    int ntr = 0, seen[4] = { 0, 0, 0, 0 };
    int cur_w = w, i, ok = 0;
    uint32_t *img;

    vbr_init(&b, off, len);

    while (vbr_bits(&b, 1)) {                    /* optional transforms    */
        int ty = (int)vbr_bits(&b, 2);
        vtrans *t;
        if (seen[ty])
            { um_set_error("duplicate VP8L transform"); goto out; }
        seen[ty] = 1;
        t = &tr[ntr++];
        t->type = ty; t->xsize = cur_w; t->data = 0; t->dn = 0; t->bits = 0;
        if (ty == 0 || ty == 1) {                /* predictor / color      */
            int sw, sh;
            t->bits = (int)vbr_bits(&b, 3) + 2;
            sw = DIVUP(cur_w, t->bits);
            sh = DIVUP(h, t->bits);
            t->data = (uint32_t *)um_alloc((unsigned long)sw * sh * 4);
            if (!t->data) { um_set_error("out of memory"); goto out; }
            if (!vp8l_data(&b, sw, sh, 0, t->data)) goto out;
        } else if (ty == 3) {                    /* color indexing         */
            int n = (int)vbr_bits(&b, 8) + 1;
            t->dn = n;
            t->data = (uint32_t *)um_alloc((unsigned long)n * 4);
            if (!t->data) { um_set_error("out of memory"); goto out; }
            if (!vp8l_data(&b, n, 1, 0, t->data)) goto out;
            for (i = 1; i < n; i++)              /* delta-coded entries    */
                t->data[i] = px_add(t->data[i], t->data[i - 1]);
            t->bits = (n <= 2) ? 3 : (n <= 4) ? 2 : (n <= 16) ? 1 : 0;
            cur_w = DIVUP(cur_w, t->bits);
        }                                        /* ty == 2: no data       */
        if (b.ovf) { um_set_error("truncated VP8L stream"); goto out; }
    }

    if (cur_w == w) {
        img = dst;
    } else {                                     /* pixel-bundled decode   */
        img = (uint32_t *)um_alloc((unsigned long)cur_w * h * 4);
        if (!img) { um_set_error("out of memory"); goto out; }
    }
    if (!vp8l_data(&b, cur_w, h, 1, img)) {
        if (img != dst) um_free(img);
        goto out;
    }

    for (i = ntr - 1; i >= 0; i--) {             /* inverses, reverse order */
        vtrans *t = &tr[i];
        switch (t->type) {
        case 0:
            if (!inv_predictor(img, cur_w, h, t)) {
                if (img != dst) um_free(img);
                goto out;
            }
            break;
        case 1:
            inv_color(img, cur_w, h, t);
            break;
        case 2:
            inv_subtract_green(img, (long)cur_w * h);
            break;
        case 3:
            if (img == dst) {                    /* width unchanged        */
                inv_index(img, cur_w, img, h, t);
            } else {
                inv_index(img, cur_w, dst, h, t);
                um_free(img);
                img = dst;
            }
            cur_w = t->xsize;
            break;
        }
    }
    ok = 1;
out:
    for (i = 0; i < ntr; i++) um_free(tr[i].data);
    return ok;
}

/* parse the 5-byte VP8L header at off (0x2f + 14+14+1+3 bits) */
static int vp8l_header(long off, long len, int *w, int *h, int *alpha)
{
    unsigned char hd[5];
    uint32_t v;
    if (len < 5 || !rdn(off, hd, 5))
        { um_set_error("truncated VP8L header"); return 0; }
    if (hd[0] != 0x2f)
        { um_set_error("bad VP8L signature"); return 0; }
    v = rd32le(hd + 1);
    *w = (int)(v & 0x3fff) + 1;
    *h = (int)((v >> 14) & 0x3fff) + 1;
    if (alpha) *alpha = (int)((v >> 28) & 1);
    if ((v >> 29) != 0)
        { um_set_error("bad VP8L version"); return 0; }
    return 1;
}

/* decode a whole VP8L chunk (header + stream) into dst as um_px */
static int vp8l_chunk(long off, long len, int w, int h, um_px *dst)
{
    int sw, sh;
    long n = (long)w * h, i;
    if (!vp8l_header(off, len, &sw, &sh, 0)) return 0;
    if (sw != w || sh != h)
        { um_set_error("WebP frame size mismatch"); return 0; }
    if (!vp8l_stream(off + 5, len - 5, w, h, (uint32_t *)dst)) return 0;
    for (i = 0; i < n; i++) {                    /* ARGB -> um_px          */
        uint32_t v = ((uint32_t *)dst)[i];
        dst[i] = (v & 0xff00ff00u) | ((v >> 16) & 0xff) | ((v & 0xff) << 16);
    }
    return 1;
}

/* ===========================================================================
 * ALPH: raw or VP8L-coded alpha plane + inverse filters 0-3, merged into
 * the frame's pixels (which arrive from the VP8 core with A = 0xff).
 * ======================================================================== */
static int apply_alpha(long off, long len, int w, int h, um_px *dst)
{
    unsigned char hd, *plane;
    int filt, comp, x, y;
    long n = (long)w * h, i;

    if (len < 1 || !rdn(off, &hd, 1))
        { um_set_error("truncated WebP alpha"); return 0; }
    filt = (hd >> 2) & 3;
    comp = hd & 3;                               /* preprocessing ignored  */
    if (comp > 1)
        { um_set_error("bad WebP alpha compression"); return 0; }

    plane = (unsigned char *)um_alloc((unsigned long)n);
    if (!plane) { um_set_error("out of memory"); return 0; }
    if (comp == 0) {                             /* raw plane              */
        if (len - 1 < n || !rdn(off + 1, plane, n))
            { um_set_error("truncated WebP alpha"); um_free(plane); return 0; }
    } else {                                     /* headerless VP8L stream */
        uint32_t *tmp = (uint32_t *)um_alloc((unsigned long)n * 4);
        if (!tmp) { um_set_error("out of memory"); um_free(plane); return 0; }
        if (!vp8l_stream(off + 1, len - 1, w, h, tmp))
            { um_free(tmp); um_free(plane); return 0; }
        for (i = 0; i < n; i++)                  /* alpha rides the green  */
            plane[i] = (unsigned char)((tmp[i] >> 8) & 0xff);
        um_free(tmp);
    }

    if (filt) {                                  /* inverse filter         */
        i = 0;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++, i++) {
                int p;
                if (y == 0 && x == 0)  p = 0;
                else if (y == 0)       p = plane[i - 1];
                else if (x == 0)       p = plane[i - w];
                else if (filt == 1)    p = plane[i - 1];
                else if (filt == 2)    p = plane[i - w];
                else p = clamp255((int)plane[i - 1] + (int)plane[i - w] -
                                  (int)plane[i - w - 1]);
                plane[i] = (unsigned char)((p + plane[i]) & 0xff);
            }
        }
    }
    for (i = 0; i < n; i++)
        dst[i] = (dst[i] & 0x00ffffffu) | ((um_px)plane[i] << 24);
    um_free(plane);
    return 1;
}

/* ---- lossy dims at open time: the core wants the whole first partition
 * in view, so hand it the whole chunk (freed before returning) ------------- */
static int lossy_dims(long off, long len, int *w, int *h)
{
    unsigned char *buf;
    int ok;
    if (len <= 0) { um_set_error("empty WebP VP8 chunk"); return 0; }
    buf = (unsigned char *)um_alloc((unsigned long)len);
    if (!buf) { um_set_error("out of memory"); return 0; }
    if (!rdn(off, buf, len))
        { um_set_error("truncated WebP"); um_free(buf); return 0; }
    ok = um_vp8_dims(buf, len, w, h);
    um_free(buf);
    return ok;
}

/* ---- one lossy frame through the VP8 core -------------------------------- */
static int lossy_decode(long off, long len, int w, int h, um_px *dst)
{
    unsigned char *buf;
    int vw, vh, ok;

    if (len <= 0) { um_set_error("empty WebP VP8 chunk"); return 0; }
    buf = (unsigned char *)um_alloc((unsigned long)len);
    if (!buf) { um_set_error("out of memory"); return 0; }
    if (!rdn(off, buf, len))
        { um_set_error("truncated WebP"); um_free(buf); return 0; }
    ok = um_vp8_dims(buf, len, &vw, &vh);
    if (ok && (vw != w || vh != h))
        { um_set_error("WebP frame size mismatch"); ok = 0; }
    if (ok) ok = um_vp8_decode(buf, len, dst);
    um_free(buf);
    return ok;
}

/* ===========================================================================
 * The RIFF container walk + animation compositing (the um_gif.c model).
 * ======================================================================== */
typedef struct {
    int  cw, ch;                /* canvas                                 */
    int  anim, frames, lossless;
    int  idx, started;
    long riff_end;
    long first_anmf, walk;      /* animation cursor                       */
    long img_off, img_len;      /* still: VP8/VP8L payload                */
    long alph_off, alph_len;    /* still: ALPH payload (0 = none)         */
    /* the previous frame's disposal, applied before the next draw        */
    int  pdisp, px, py, pw, ph;
} webp_st;

static webp_st *st;

/* chunk header at off: 1 = ok, 0 = clean end, -1 = size overruns the file */
static int chead(long off, long end, unsigned char cc[4], long *paylen)
{
    unsigned char h[8];
    if (off + 8 > end) return 0;
    if (!rdn(off, h, 8)) return 0;
    memcpy(cc, h, 4);
    if ((long long)rd32le(h + 4) > (long long)(end - (off + 8))) return -1;
    *paylen = (long)rd32le(h + 4);
    return 1;
}

static int dims_ok(long w, long h)
{
    return w > 0 && h > 0 && w <= UM_MAX_DIM && h <= UM_MAX_DIM &&
           w * h <= UM_MAX_PIXELS;
}

static void webp_close(void)
{
    um_free(st);
    st = 0;
}

/* ---- animation census: exact frame count + alpha spotting, no pixels ----- */
static int anim_census(long from, int aflag, int *alpha)
{
    unsigned char cc[4], fh[16];
    long o = from, plen;
    int r, have_anim = 0, covered = 0, bgdisp = 0;

    st->frames = 0;
    while ((r = chead(o, st->riff_end, cc, &plen)) == 1) {
        if (!memcmp(cc, "ANIM", 4)) {
            if (plen < 6)
                { um_set_error("corrupt WebP ANIM chunk"); return 0; }
            have_anim = 1;
        } else if (!memcmp(cc, "ANMF", 4)) {
            long fx, fy, fw, fhh;
            if (!have_anim)
                { um_set_error("WebP animation missing ANIM chunk"); return 0; }
            if (plen < 16 || !rdn(o + 8, fh, 16))
                { um_set_error("corrupt WebP ANMF chunk"); return 0; }
            fx = 2L * (long)rd24le(fh);
            fy = 2L * (long)rd24le(fh + 3);
            fw = (long)rd24le(fh + 6) + 1;
            fhh = (long)rd24le(fh + 9) + 1;
            if (fx + fw > st->cw || fy + fhh > st->ch)
                { um_set_error("WebP frame outside canvas"); return 0; }
            if (st->frames == 0) {
                st->first_anmf = o;
                covered = (fx == 0 && fy == 0 &&
                           fw == st->cw && fhh == st->ch);
            }
            if (fh[15] & 1) bgdisp = 1;
            st->frames++;
        }
        o += 8 + plen + (plen & 1);
    }
    if (r < 0) { um_set_error("WebP chunk overruns file"); return 0; }
    if (!st->frames)
        { um_set_error("WebP animation has no frames"); return 0; }
    *alpha = aflag || !covered || bgdisp;
    return 1;
}

static int webp_open(um_image_info *info)
{
    unsigned char hd[12], cc[4], xb[10];
    long long re;
    long plen;
    int w, h, hint = 0, alpha = 0;

    webp_close();
    if (!rdn(0, hd, 12) || memcmp(hd, "RIFF", 4) || memcmp(hd + 8, "WEBP", 4))
        { um_set_error("truncated WebP header"); return 0; }
    st = (webp_st *)um_alloc(sizeof *st);
    if (!st) { um_set_error("out of memory"); return 0; }
    memset(st, 0, sizeof *st);
    re = 8 + (long long)rd32le(hd + 4);
    if (re > um_size()) re = um_size();          /* trust the file, capped */
    st->riff_end = (long)re;

    if (chead(12, st->riff_end, cc, &plen) != 1)
        { um_set_error("truncated WebP"); webp_close(); return 0; }

    if (!memcmp(cc, "VP8X", 4)) {                /* extended layout        */
        int aflag, anim;
        if (plen < 10 || !rdn(20, xb, 10))
            { um_set_error("corrupt WebP VP8X chunk"); webp_close(); return 0; }
        aflag = (xb[0] >> 4) & 1;
        anim  = (xb[0] >> 1) & 1;
        st->cw = (int)rd24le(xb + 4) + 1;
        st->ch = (int)rd24le(xb + 7) + 1;
        if (!dims_ok(st->cw, st->ch))
            { um_set_error("WebP dimensions out of range"); webp_close(); return 0; }
        st->walk = 20 + plen + (plen & 1);

        if (anim) {
            st->anim = 1;
            if (!anim_census(st->walk, aflag, &alpha))
                { webp_close(); return 0; }
            st->walk = st->first_anmf;
            info->bpp = 32;
        } else {                                 /* still, maybe with ALPH */
            long o = st->walk;
            int r;
            while ((r = chead(o, st->riff_end, cc, &plen)) == 1) {
                if (!memcmp(cc, "ALPH", 4)) {
                    st->alph_off = o + 8;
                    st->alph_len = plen;
                } else if (!memcmp(cc, "VP8 ", 4) || !memcmp(cc, "VP8L", 4)) {
                    st->img_off = o + 8;
                    st->img_len = plen;
                    st->lossless = cc[3] == 'L';
                    break;
                }
                o += 8 + plen + (plen & 1);
            }
            if (r < 0)
                { um_set_error("WebP chunk overruns file"); webp_close(); return 0; }
            if (!st->img_len)
                { um_set_error("WebP has no image data"); webp_close(); return 0; }
            if (st->lossless) {
                st->alph_len = 0;                /* VP8L carries its own   */
                if (!vp8l_header(st->img_off, st->img_len, &w, &h, &hint))
                    { webp_close(); return 0; }
            } else {
                if (!lossy_dims(st->img_off, st->img_len, &w, &h))
                    { webp_close(); return 0; }
            }
            if (w != st->cw || h != st->ch)
                { um_set_error("WebP canvas/frame size mismatch"); webp_close(); return 0; }
            alpha = aflag || st->alph_len > 0 || hint;
            info->bpp = (st->lossless || st->alph_len) ? 32 : 24;
        }
        st->frames = st->anim ? st->frames : 1;
    } else if (!memcmp(cc, "VP8L", 4)) {         /* simple lossless        */
        st->img_off = 20;
        st->img_len = plen;
        st->lossless = 1;
        st->frames = 1;
        if (!vp8l_header(20, plen, &st->cw, &st->ch, &hint))
            { webp_close(); return 0; }
        alpha = hint;
        info->bpp = 32;
    } else if (!memcmp(cc, "VP8 ", 4)) {         /* simple lossy           */
        st->img_off = 20;
        st->img_len = plen;
        st->frames = 1;
        if (!lossy_dims(20, plen, &st->cw, &st->ch))
            { webp_close(); return 0; }
        info->bpp = 24;
    } else {
        um_set_error("corrupt WebP container");
        webp_close();
        return 0;
    }

    if (!dims_ok(st->cw, st->ch))
        { um_set_error("WebP dimensions out of range"); webp_close(); return 0; }
    info->format = "WEBP";
    info->w = st->cw;
    info->h = st->ch;
    info->alpha = alpha;
    info->frames = st->frames;
    return 1;
}

/* the spec's straight-alpha "over" (integer form; opaque paths exact) */
static um_px blend_px(um_px s, um_px d)
{
    unsigned sa = s >> 24, da = d >> 24, ba, sh;
    um_px out;
    if (sa == 255 || da == 0) return s;
    if (sa == 0) return d;
    ba = sa + da * (255 - sa) / 255;
    if (ba == 0) return 0;
    out = (um_px)ba << 24;
    for (sh = 0; sh < 24; sh += 8) {
        unsigned sc = (s >> sh) & 0xff, dc = (d >> sh) & 0xff;
        out |= (um_px)((sc * sa + dc * da * (255 - sa) / 255) / ba) << sh;
    }
    return out;
}

static int webp_frame(um_px *dst, int *delay_ms)
{
    unsigned char cc[4], fh[16];
    long plen, slen, o, end, ioff = 0, ilen = 0, aoff = 0, alen = 0;
    int fx, fy, fw, fhh, flags, lossless = 0, ok, y, r;
    um_px *fbuf;

    if (!st) { um_set_error("WebP decoder not open"); return -1; }

    if (!st->anim) {                             /* still: one frame       */
        if (st->idx) return 0;
        if (st->lossless) {
            if (!vp8l_chunk(st->img_off, st->img_len, st->cw, st->ch, dst))
                return -1;
        } else {
            if (!lossy_decode(st->img_off, st->img_len, st->cw, st->ch, dst))
                return -1;
            if (st->alph_len &&
                !apply_alpha(st->alph_off, st->alph_len, st->cw, st->ch, dst))
                return -1;
        }
        *delay_ms = 0;
        st->idx = 1;
        return 1;
    }

    if (st->idx == 0 && !st->started) {          /* fresh playthrough      */
        memset(dst, 0, (unsigned long)st->cw * st->ch * 4);
        st->started = 1;
    }
    if (st->idx >= st->frames) return 0;

    r = chead(st->walk, st->riff_end, cc, &plen);
    if (r != 1 || memcmp(cc, "ANMF", 4) || plen < 16 ||
        !rdn(st->walk + 8, fh, 16))
        { um_set_error("corrupt WebP animation"); return -1; }
    fx  = 2 * (int)rd24le(fh);
    fy  = 2 * (int)rd24le(fh + 3);
    fw  = (int)rd24le(fh + 6) + 1;
    fhh = (int)rd24le(fh + 9) + 1;
    flags = fh[15];
    if ((long)fx + fw > st->cw || (long)fy + fhh > st->ch)
        { um_set_error("WebP frame outside canvas"); return -1; }

    /* the previous frame's dispose-to-background: clear to transparent   */
    if (st->idx > 0 && st->pdisp)
        for (y = 0; y < st->ph; y++)
            memset(dst + (long)(st->py + y) * st->cw + st->px, 0,
                   (unsigned long)st->pw * 4);

    o = st->walk + 8 + 16;                       /* frame data subchunks   */
    end = st->walk + 8 + plen;
    while ((r = chead(o, end, cc, &slen)) == 1) {
        if (!memcmp(cc, "ALPH", 4)) {
            aoff = o + 8; alen = slen;
        } else if (!memcmp(cc, "VP8 ", 4) || !memcmp(cc, "VP8L", 4)) {
            ioff = o + 8; ilen = slen;
            lossless = cc[3] == 'L';
            break;
        }
        o += 8 + slen + (slen & 1);
    }
    if (r < 0) { um_set_error("WebP chunk overruns file"); return -1; }
    if (!ilen) { um_set_error("WebP frame has no image data"); return -1; }

    fbuf = (um_px *)um_alloc((unsigned long)fw * fhh * 4);
    if (!fbuf) { um_set_error("out of memory"); return -1; }
    if (lossless) {
        ok = vp8l_chunk(ioff, ilen, fw, fhh, fbuf);
    } else {
        ok = lossy_decode(ioff, ilen, fw, fhh, fbuf);
        if (ok && alen) ok = apply_alpha(aoff, alen, fw, fhh, fbuf);
    }
    if (!ok) { um_free(fbuf); return -1; }

    for (y = 0; y < fhh; y++) {                  /* composite the rect     */
        um_px *drow = dst + (long)(fy + y) * st->cw + fx;
        const um_px *srow = fbuf + (long)y * fw;
        if (flags & 2) {                         /* no-blend: overwrite    */
            memcpy(drow, srow, (unsigned long)fw * 4);
        } else {
            int x;
            for (x = 0; x < fw; x++)
                drow[x] = blend_px(srow[x], drow[x]);
        }
    }
    um_free(fbuf);

    st->pdisp = flags & 1;
    st->px = fx; st->py = fy; st->pw = fw; st->ph = fhh;
    *delay_ms = (int)rd24le(fh + 12);
    st->idx++;
    st->walk = end + (plen & 1);                 /* past the padded ANMF   */
    return 1;
}

static void webp_rewind(void)
{
    if (!st) return;
    st->idx = 0;
    st->started = 0;
    st->pdisp = 0;
    st->walk = st->anim ? st->first_anmf : 0;
}

static int webp_probe(const unsigned char *head, long n, const char *ext)
{
    (void)ext;
    return n >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WEBP", 4);
}

const um_idecoder um_idec_webp =
    { "webp", webp_probe, webp_open, webp_frame, webp_rewind, webp_close };

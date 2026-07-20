/* ===========================================================================
 * unomedia - streaming inflate (RFC 1950 / RFC 1951), written from scratch.
 *
 * PNG's engine, but a general facility: nothing in here knows about PNG.
 * The caller supplies a pull callback for compressed bytes and a push
 * callback for decompressed runs; all working state - a 32 KB LZ77 window,
 * a small input buffer, three canonical Huffman tables - lives in one
 * um_alloc'd block that is freed before um_inflate returns, so the resident
 * cost between calls is zero and .bss holds only .rodata spec tables.
 *
 * All three block types are decoded: stored, fixed-Huffman, and dynamic-
 * Huffman. Huffman codes are decoded bit-serially against the canonical
 * (count-per-length, first-code-per-length) form that RFC 1951 3.2.2
 * defines - slower than a lookup table but small, obviously correct, and
 * plenty for image-sized streams. Decoded bytes accumulate in the window
 * and are pushed out in up-to-32 KB runs each time it wraps (and once at
 * the end), so back-references always have their full history in place.
 *
 * DELIBERATE LIMITS:
 *   - zlib mode checks the RFC 1950 header (method 8, window <= 32 KB,
 *     FCHECK) and refuses FDICT, but does NOT verify the trailing adler32:
 *     integrity checking belongs to the transport, and every structural
 *     error a corrupt stream can cause is already caught below. The four
 *     trailer bytes are simply never read.
 *   - Incomplete Huffman code sets are accepted at build time (the spec's
 *     own one-distance-code case requires it); using a code the set does
 *     not define fails at decode time instead.
 *   - When the output callback aborts (returns 0), um_inflate returns 0
 *     WITHOUT touching um_error - the aborting callback owns the reason.
 *
 * The length/distance base+extra tables and the fixed-tree code lengths
 * below are data the spec itself defines (RFC 1951 3.2.5 / 3.2.6).
 * ======================================================================== */
#include "unomedia.h"
#include <string.h>
#include <stdint.h>

#define WIN    32768                /* LZ77 window, the format maximum      */
#define WMASK  (WIN - 1)
#define INBUF  2048                 /* compressed-input refill unit         */
#define MAXSYM 288                  /* literal/length alphabet size         */

/* internal result codes (negative so they never collide with symbols) */
#define I_OK     0
#define I_EOF   (-1)                /* input ran out mid-stream             */
#define I_BAD   (-2)                /* malformed data                       */
#define I_ABORT (-3)                /* output callback said stop            */

/* a canonical Huffman code set: how many codes of each length, and the
 * symbols ordered by (length, symbol) - all decode needs (RFC 1951 3.2.2) */
typedef struct {
    uint16_t count[16];             /* codes per bit length 0..15           */
    uint16_t sym[MAXSYM];           /* symbols in canonical order           */
} huf_t;

typedef struct {
    um_inf_in_fn  in;  void *in_ctx;
    um_inf_out_fn out; void *out_ctx;
    unsigned char inbuf[INBUF];
    long          in_len, in_pos;
    int           in_eof;
    uint32_t      bitbuf;           /* bits, LSB next (RFC 1951 3.1.1)      */
    int           bitcnt;
    unsigned char win[WIN];
    unsigned long wpos;             /* next write index in win              */
    unsigned long total;            /* bytes produced so far                */
    huf_t         lit, dist, clc;
    unsigned char lens[MAXSYM + 32];/* scratch for building code sets       */
} inf_t;

/* ---- spec constant tables (RFC 1951 3.2.5) -------------------------------- */
static const uint16_t k_lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char k_lxtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
static const uint16_t k_dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char k_dxtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
    10, 11, 11, 12, 12, 13, 13
};

/* ---- input ---------------------------------------------------------------- */
static int inf_byte(inf_t *z)
{
    if (z->in_pos >= z->in_len) {
        long n;
        if (z->in_eof) return I_EOF;
        n = z->in(z->in_ctx, z->inbuf, INBUF);
        if (n <= 0) { z->in_eof = 1; return I_EOF; }
        z->in_len = n;
        z->in_pos = 0;
    }
    return z->inbuf[z->in_pos++];
}

/* n (0..16) bits, LSB-first per RFC 1951 3.1.1; >= 0 value or I_EOF */
static int inf_bits(inf_t *z, int n)
{
    int v;
    while (z->bitcnt < n) {
        int b = inf_byte(z);
        if (b < 0) return I_EOF;
        z->bitbuf |= (uint32_t)b << z->bitcnt;
        z->bitcnt += 8;
    }
    v = (int)(z->bitbuf & (((uint32_t)1 << n) - 1));
    z->bitbuf >>= n;
    z->bitcnt -= n;
    return v;
}

/* ---- output through the window -------------------------------------------- */
static int inf_put(inf_t *z, int b)
{
    z->win[z->wpos++] = (unsigned char)b;
    z->total++;
    if (z->wpos == WIN) {
        z->wpos = 0;
        if (!z->out(z->out_ctx, z->win, WIN)) return I_ABORT;
    }
    return I_OK;
}

/* ---- canonical Huffman ---------------------------------------------------- */
/* build from a length-per-symbol list; fails only when over-subscribed */
static int huf_build(huf_t *h, const unsigned char *lens, int n)
{
    uint16_t offs[16];
    int i, left;

    for (i = 0; i < 16; i++) h->count[i] = 0;
    for (i = 0; i < n; i++) h->count[lens[i]]++;
    if (h->count[0] == n) return 0;         /* empty set: legal, unusable   */

    left = 1;                               /* codes still available        */
    for (i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return -1;            /* more codes than bits allow   */
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (uint16_t)i;
    return 0;
}

/* decode one symbol bit-serially: at each length, the canonical numbering
 * means codes of that length occupy [first, first+count) */
static int huf_dec(inf_t *z, const huf_t *h)
{
    int len, code = 0, first = 0, index = 0;

    for (len = 1; len <= 15; len++) {
        int cnt, b = inf_bits(z, 1);
        if (b < 0) return I_EOF;
        code |= b;
        cnt = h->count[len];
        if (code - first < cnt) return h->sym[index + (code - first)];
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return I_BAD;                           /* not a code in this set       */
}

/* ---- block types ---------------------------------------------------------- */
static int inf_stored(inf_t *z)
{
    int b0, b1, len, nlen;
    int k = z->bitcnt & 7;                  /* discard to byte boundary     */
    z->bitbuf >>= k;
    z->bitcnt -= k;

    b0 = inf_bits(z, 8); b1 = inf_bits(z, 8);
    if (b0 < 0 || b1 < 0) return I_EOF;
    len = b0 | (b1 << 8);
    b0 = inf_bits(z, 8); b1 = inf_bits(z, 8);
    if (b0 < 0 || b1 < 0) return I_EOF;
    nlen = b0 | (b1 << 8);
    if ((len ^ nlen) != 0xFFFF) return I_BAD;

    while (len--) {
        int r, b = (z->bitcnt >= 8) ? inf_bits(z, 8) : inf_byte(z);
        if (b < 0) return I_EOF;
        r = inf_put(z, b);
        if (r) return r;
    }
    return I_OK;
}

/* the shared literal/length + distance loop (RFC 1951 3.2.3) */
static int inf_codes(inf_t *z)
{
    for (;;) {
        int sym = huf_dec(z, &z->lit);
        if (sym < 0) return sym;
        if (sym < 256) {
            int r = inf_put(z, sym);
            if (r) return r;
        } else if (sym == 256) {
            return I_OK;                    /* end of block                 */
        } else {
            int e, len, dist, r;
            unsigned long si;
            sym -= 257;
            if (sym >= 29) return I_BAD;    /* 286/287 are reserved         */
            e = inf_bits(z, k_lxtra[sym]);
            if (e < 0) return I_EOF;
            len = k_lbase[sym] + e;
            sym = huf_dec(z, &z->dist);
            if (sym < 0) return sym;
            if (sym >= 30) return I_BAD;    /* 30/31 are reserved           */
            e = inf_bits(z, k_dxtra[sym]);
            if (e < 0) return I_EOF;
            dist = k_dbase[sym] + e;
            if ((unsigned long)dist > z->total)
                return I_BAD;               /* before the stream started    */
            si = (z->wpos + WIN - (unsigned long)dist) & WMASK;
            while (len--) {                 /* byte-serial: overlap-correct */
                r = inf_put(z, z->win[si]);
                if (r) return r;
                si = (si + 1) & WMASK;
            }
        }
    }
}

/* fixed trees: code lengths straight from RFC 1951 3.2.6 (cannot fail) */
static void inf_fixed(inf_t *z)
{
    int i;
    for (i = 0;   i < 144; i++) z->lens[i] = 8;
    for (;        i < 256; i++) z->lens[i] = 9;
    for (;        i < 280; i++) z->lens[i] = 7;
    for (;        i < 288; i++) z->lens[i] = 8;
    (void)huf_build(&z->lit, z->lens, 288);
    for (i = 0; i < 32; i++) z->lens[i] = 5;
    (void)huf_build(&z->dist, z->lens, 32);
}

/* dynamic trees: the code-length code, then run-length coded lengths
 * (RFC 1951 3.2.7) */
static int inf_dynamic(inf_t *z)
{
    static const unsigned char ord[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    int hlit, hdist, hclen, i, n;

    hlit  = inf_bits(z, 5);
    hdist = inf_bits(z, 5);
    hclen = inf_bits(z, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return I_EOF;
    hlit += 257; hdist += 1; hclen += 4;
    if (hlit > 286) return I_BAD;

    for (i = 0; i < hclen; i++) {
        int v = inf_bits(z, 3);
        if (v < 0) return I_EOF;
        z->lens[ord[i]] = (unsigned char)v;
    }
    for (; i < 19; i++) z->lens[ord[i]] = 0;
    if (huf_build(&z->clc, z->lens, 19)) return I_BAD;

    n = 0;
    while (n < hlit + hdist) {
        int sym = huf_dec(z, &z->clc);
        if (sym < 0) return sym;
        if (sym < 16) {
            z->lens[n++] = (unsigned char)sym;
        } else {
            int e, rep, val = 0;
            if (sym == 16) {                /* repeat previous length       */
                if (n == 0) return I_BAD;
                val = z->lens[n - 1];
                e = inf_bits(z, 2);
                if (e < 0) return I_EOF;
                rep = 3 + e;
            } else if (sym == 17) {         /* short run of zeros           */
                e = inf_bits(z, 3);
                if (e < 0) return I_EOF;
                rep = 3 + e;
            } else {                        /* 18: long run of zeros        */
                e = inf_bits(z, 7);
                if (e < 0) return I_EOF;
                rep = 11 + e;
            }
            if (n + rep > hlit + hdist) return I_BAD;
            while (rep--) z->lens[n++] = (unsigned char)val;
        }
    }
    if (z->lens[256] == 0) return I_BAD;    /* no end-of-block code         */
    if (huf_build(&z->lit,  z->lens,        hlit))  return I_BAD;
    if (huf_build(&z->dist, z->lens + hlit, hdist)) return I_BAD;
    return I_OK;
}

/* ---- the public entry ----------------------------------------------------- */
int um_inflate(um_inf_in_fn in, void *in_ctx,
               um_inf_out_fn out, void *out_ctx, int zlib)
{
    inf_t *z;
    int r = I_OK, final;

    if (!in || !out) { um_set_error("inflate: bad call"); return 0; }
    z = (inf_t *)um_alloc(sizeof *z);
    if (!z) { um_set_error("inflate: out of memory"); return 0; }
    memset(z, 0, sizeof *z);
    z->in = in;  z->in_ctx = in_ctx;
    z->out = out; z->out_ctx = out_ctx;

    if (zlib) {                             /* RFC 1950 header              */
        int cmf = inf_byte(z), flg = inf_byte(z);
        if (cmf < 0 || flg < 0) {
            um_free(z); um_set_error("zlib stream truncated"); return 0;
        }
        if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 ||
            ((cmf << 8) | flg) % 31 != 0) {
            um_free(z); um_set_error("bad zlib header"); return 0;
        }
        if (flg & 0x20) {
            um_free(z);
            um_set_error("zlib preset dictionary not supported");
            return 0;
        }
    }

    do {
        int type;
        final = inf_bits(z, 1);
        type  = inf_bits(z, 2);
        if (final < 0 || type < 0) { r = I_EOF; break; }
        if      (type == 0) r = inf_stored(z);
        else if (type == 1) { inf_fixed(z); r = inf_codes(z); }
        else if (type == 2) { r = inf_dynamic(z); if (!r) r = inf_codes(z); }
        else                r = I_BAD;      /* block type 3 is reserved     */
        if (r) break;
    } while (!final);

    if (r == I_OK && z->wpos) {             /* the unwrapped tail           */
        if (!z->out(z->out_ctx, z->win, (long)z->wpos)) r = I_ABORT;
    }
    /* trailing adler32 (zlib mode) deliberately unread - see the banner */
    um_free(z);

    if (r == I_OK) return 1;
    if (r == I_EOF)      um_set_error("truncated deflate stream");
    else if (r == I_BAD) um_set_error("malformed deflate stream");
    /* I_ABORT: the output callback already set the reason */
    return 0;
}

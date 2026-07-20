/* ===========================================================================
 * unomedia - the PNG decoder, written from scratch against the PNG spec
 * (ISO/IEC 15948 / RFC 2083).
 *
 * Everything a baseline still needs: colour types 0/2/3/4/6 at bit depths
 * 1/2/4/8/16 (every combination the spec allows), all five scanline
 * filters, Adam7 interlace, PLTE, tRNS in all three shapes (palette alpha,
 * grey colourkey, RGB colourkey), and IDAT split across any number of
 * chunks. 16-bit samples fold to 8 by the exact 255/65535 scale - since
 * 65535 = 257 * 255 that is just v / 257, no float needed - because the
 * output surface is 8-bit um_px. Unknown and ancillary chunks are skipped
 * unread.
 *
 * MEMORY SHAPE: the image is never buffered decompressed. IDAT streams
 * through um_inflate; the output callback gathers scanline records into a
 * two-row (cur/prev) buffer, unfilters, and converts pixels straight into
 * the caller's dst. Adam7 needs no intermediate either: each reduced-image
 * pixel scatters directly to its final (x0 + x*dx, y0 + y*dy) cell, so the
 * resident cost is two rows regardless of image size.
 *
 * DELIBERATE LIMITS:
 *   - Chunk CRCs are never verified (and inflate skips adler32): integrity
 *     belongs to the transport, and corrupt data still fails cleanly on
 *     structure - bad filter bytes, bad Huffman codes, short streams,
 *     out-of-range palette indexes are all hard errors.
 *   - gAMA/iCCP/sRGB are ignored: samples pass through unmanaged, the same
 *     policy every other unomedia decoder applies.
 *   - APNG (acTL/fcTL/fdAT) is not parsed; such a file decodes as its
 *     spec-mandated fallback still. frames is always 1.
 *
 * um_png_open_at(off, info) opens a PNG stream embedded at byte offset
 * `off` (the ICO delegation hook, see unomedia_int.h); every file offset
 * in here is computed from that base. The ordinary open() is offset 0.
 * ======================================================================== */
#include "unomedia.h"
#include "unomedia_int.h"
#include <string.h>
#include <stdint.h>

/* ---- decoder state (one open image, the unomedia convention) -------------- */
typedef struct {
    int   open;
    long  base;                 /* absolute offset of the PNG signature     */
    long  w, h;
    int   depth, ctype, interlace;
    int   nch;                  /* samples per pixel                        */
    int   fu;                   /* filter unit: bytes per pixel, min 1      */
    long  first_idat;           /* absolute offset of first IDAT header     */
    /* palette + transparency */
    um_px    pal[256];
    int      pal_n;
    int      trns;              /* tRNS present (any form)                  */
    unsigned kr, kg, kb;        /* colourkey samples at source depth        */
    /* IDAT walk (live during a decode) */
    long  id_off, id_rem, id_next;
    /* scanline machine */
    unsigned char *cur, *prev;  /* rowmax bytes each, from um_alloc         */
    long  rowmax;               /* rowbytes of a full-width scanline        */
    int   pass;                 /* 0..6 = Adam7 pass, 7 = whole image       */
    long  pw, ph, prb;          /* current pass width/height/rowbytes       */
    long  y;                    /* row within the pass                      */
    long  have;                 /* bytes gathered of the current record     */
    int   filt;                 /* the record's filter byte                 */
    int   done;                 /* every row delivered                      */
    int   derr;                 /* conversion error (um_error already set)  */
    um_px *dst;
    int   played;               /* the single frame has been returned       */
} png_t;

static png_t P;

/* Adam7 pass origins and steps (PNG spec 8.2); index 7 = the whole image */
static const unsigned char a7x0[8] = { 0, 4, 0, 2, 0, 1, 0, 0 };
static const unsigned char a7y0[8] = { 0, 0, 4, 0, 2, 0, 1, 0 };
static const unsigned char a7dx[8] = { 8, 8, 4, 4, 2, 2, 1, 1 };
static const unsigned char a7dy[8] = { 8, 8, 8, 4, 4, 2, 2, 1 };

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* ---- sample access -------------------------------------------------------- */
/* raw sample i of a scanline (bit-packed MSB-first below 8, big-endian 16) */
static unsigned samp(const unsigned char *row, long i)
{
    switch (P.depth) {
    case 1:  return (unsigned)(row[i >> 3] >> (7 - (i & 7))) & 1u;
    case 2:  return (unsigned)(row[i >> 2] >> ((3 - (i & 3)) * 2)) & 3u;
    case 4:  return (unsigned)(row[i >> 1] >> ((1 - (i & 1)) * 4)) & 15u;
    case 8:  return row[i];
    default: return ((unsigned)row[i * 2] << 8) | row[i * 2 + 1];
    }
}

/* scale a raw sample to 8 bits (exact for <8: v * 255/(2^d-1); fold for 16) */
static unsigned to8(unsigned v)
{
    switch (P.depth) {
    case 1:  return v * 255u;
    case 2:  return v * 85u;
    case 4:  return v * 17u;
    case 8:  return v;
    default: return v / 257;    /* floor(v * 255/65535), since 65535=257*255 */
    }
}

/* ---- scanline machine ----------------------------------------------------- */
/* enter the next pass (from `p` up) that actually has pixels; 0 = finished */
static int pass_begin(int p)
{
    int last = P.interlace ? 6 : 7;
    if (!P.interlace && p < 7) p = 7;
    for (; p <= last; p++) {
        long pw = (P.w > a7x0[p]) ? (P.w - a7x0[p] + a7dx[p] - 1) / a7dx[p] : 0;
        long ph = (P.h > a7y0[p]) ? (P.h - a7y0[p] + a7dy[p] - 1) / a7dy[p] : 0;
        if (pw <= 0 || ph <= 0) continue;
        P.pass = p;
        P.pw = pw;
        P.ph = ph;
        P.prb = (pw * P.nch * P.depth + 7) / 8;
        P.y = 0;
        P.have = 0;
        memset(P.prev, 0, (size_t)P.rowmax);    /* row -1 is all zeros      */
        return 1;
    }
    return 0;
}

static int paeth(int a, int b, int c)
{
    int p  = a + b - c;
    int pa = p - a; if (pa < 0) pa = -pa;
    int pb = p - b; if (pb < 0) pb = -pb;
    int pc = p - c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* undo the record's filter in place (PNG spec 9.2); left/up-left read as 0
 * for the first fu bytes, so the special-cased prefixes below fall out */
static int unfilter(void)
{
    unsigned char *c = P.cur;
    const unsigned char *p = P.prev;
    long i, n = P.prb;
    int fu = P.fu;

    switch (P.filt) {
    case 0:                                             /* None             */
        break;
    case 1:                                             /* Sub              */
        for (i = fu; i < n; i++)
            c[i] = (unsigned char)(c[i] + c[i - fu]);
        break;
    case 2:                                             /* Up               */
        for (i = 0; i < n; i++)
            c[i] = (unsigned char)(c[i] + p[i]);
        break;
    case 3:                                             /* Average          */
        for (i = 0; i < fu && i < n; i++)
            c[i] = (unsigned char)(c[i] + (p[i] >> 1));
        for (i = fu; i < n; i++)
            c[i] = (unsigned char)(c[i] + ((c[i - fu] + p[i]) >> 1));
        break;
    case 4:                                             /* Paeth            */
        for (i = 0; i < fu && i < n; i++)
            c[i] = (unsigned char)(c[i] + p[i]);
        for (i = fu; i < n; i++)
            c[i] = (unsigned char)(c[i] + paeth(c[i - fu], p[i], p[i - fu]));
        break;
    default:
        um_set_error("PNG bad scanline filter type");
        return 0;
    }
    return 1;
}

/* convert the unfiltered row and scatter it into dst at its pass geometry */
static int emit_row(void)
{
    const unsigned char *r = P.cur;
    long dx = a7dx[P.pass];
    um_px *d = P.dst +
        ((long)a7y0[P.pass] + P.y * a7dy[P.pass]) * P.w + a7x0[P.pass];
    long x;

    switch (P.ctype) {
    case 0:                                             /* greyscale        */
        for (x = 0; x < P.pw; x++) {
            unsigned v = samp(r, x);
            unsigned a = (P.trns && v == P.kr) ? 0u : 255u;
            unsigned g = to8(v);
            d[x * dx] = UM_PX(g, g, g, a);
        }
        break;
    case 2:                                             /* RGB              */
        for (x = 0; x < P.pw; x++) {
            unsigned rv = samp(r, x * 3);
            unsigned gv = samp(r, x * 3 + 1);
            unsigned bv = samp(r, x * 3 + 2);
            unsigned a  = (P.trns && rv == P.kr && gv == P.kg && bv == P.kb)
                          ? 0u : 255u;
            d[x * dx] = UM_PX(to8(rv), to8(gv), to8(bv), a);
        }
        break;
    case 3:                                             /* palette          */
        for (x = 0; x < P.pw; x++) {
            unsigned i = samp(r, x);
            if ((int)i >= P.pal_n) {
                um_set_error("PNG palette index out of range");
                return 0;
            }
            d[x * dx] = P.pal[i];
        }
        break;
    case 4:                                             /* grey + alpha     */
        for (x = 0; x < P.pw; x++) {
            unsigned g = to8(samp(r, x * 2));
            unsigned a = to8(samp(r, x * 2 + 1));
            d[x * dx] = UM_PX(g, g, g, a);
        }
        break;
    default:                                            /* 6: RGBA          */
        for (x = 0; x < P.pw; x++) {
            unsigned rv = to8(samp(r, x * 4));
            unsigned gv = to8(samp(r, x * 4 + 1));
            unsigned bv = to8(samp(r, x * 4 + 2));
            unsigned av = to8(samp(r, x * 4 + 3));
            d[x * dx] = UM_PX(rv, gv, bv, av);
        }
        break;
    }
    return 1;
}

/* ---- um_inflate callbacks ------------------------------------------------- */
/* pull compressed bytes out of the IDAT chunk run (consecutive per spec) */
static long png_in(void *ctx, unsigned char *dst, long max)
{
    (void)ctx;
    for (;;) {
        if (P.id_rem > 0) {
            long take = (max < P.id_rem) ? max : P.id_rem;
            long r = um_read(P.id_off, dst, take);
            if (r <= 0) return 0;           /* file shorter than the chunk  */
            P.id_off += r;
            P.id_rem -= r;
            return r;
        }
        {                                   /* step to the next chunk       */
            unsigned char h[8];
            uint32_t len;
            if (P.id_next < 0) return 0;
            if (um_read(P.id_next, h, 8) != 8) return 0;
            if (memcmp(h + 4, "IDAT", 4)) { P.id_next = -1; return 0; }
            len = be32(h);
            if (len > 0x7FFFFF00u) return -1;
            P.id_off  = P.id_next + 8;
            P.id_rem  = (long)len;          /* 0-length IDAT: loop again    */
            P.id_next = P.id_off + (long)len + 4;   /* skip data + CRC      */
        }
    }
}

/* push decompressed bytes through the record gatherer */
static int png_out(void *ctx, const unsigned char *b, long n)
{
    (void)ctx;
    while (n > 0) {
        long need, take;
        if (P.done) return 1;               /* swallow any trailing bytes   */
        if (P.have == 0) {                  /* record starts with a filter  */
            P.filt = *b++;
            n--;
            P.have = 1;
            continue;
        }
        need = 1 + P.prb - P.have;
        take = (n < need) ? n : need;
        memcpy(P.cur + (P.have - 1), b, (size_t)take);
        b += take;
        n -= take;
        P.have += take;
        if (P.have == 1 + P.prb) {          /* a whole record: process it   */
            unsigned char *t;
            if (!unfilter() || !emit_row()) { P.derr = 1; return 0; }
            t = P.cur; P.cur = P.prev; P.prev = t;
            P.have = 0;
            if (++P.y >= P.ph && !pass_begin(P.pass + 1))
                P.done = 1;
        }
    }
    return 1;
}

/* ---- the vtable ----------------------------------------------------------- */
static void png_close(void)
{
    um_free(P.cur);
    um_free(P.prev);
    P.cur = P.prev = 0;                     /* idempotent: nothing dangles  */
    P.open = 0;
    P.played = 0;
}

int um_png_open_at(long off, um_image_info *info)
{
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned char b[26];
    uint32_t w, h;
    long pos;
    int dok = 0, seen_plte = 0;

    png_close();
    memset(&P, 0, sizeof P);

    if (um_read(off, b, 8) != 8 || memcmp(b, sig, 8)) {
        um_set_error("PNG signature missing");
        return 0;
    }
    /* IHDR must be the very first chunk */
    if (um_read(off + 8, b, 25) != 25) {
        um_set_error("PNG truncated in IHDR");
        return 0;
    }
    if (be32(b) != 13 || memcmp(b + 4, "IHDR", 4)) {
        um_set_error("PNG IHDR missing");
        return 0;
    }
    w = be32(b + 8);
    h = be32(b + 12);
    P.depth = b[16];
    P.ctype = b[17];
    if (b[18] != 0) { um_set_error("PNG bad compression method"); return 0; }
    if (b[19] != 0) { um_set_error("PNG bad filter method");      return 0; }
    if (b[20] > 1)  { um_set_error("PNG bad interlace method");   return 0; }
    P.interlace = b[20];

    /* believe nothing: dimension guards BEFORE any size arithmetic */
    if (w == 0 || h == 0 || w > UM_MAX_DIM || h > UM_MAX_DIM ||
        (long)w * (long)h > UM_MAX_PIXELS) {
        um_set_error("image dimensions out of range");
        return 0;
    }

    switch (P.ctype) {                      /* spec-legal depth sets only   */
    case 0: P.nch = 1;
        dok = (P.depth == 1 || P.depth == 2 || P.depth == 4 ||
               P.depth == 8 || P.depth == 16);
        break;
    case 2: P.nch = 3; dok = (P.depth == 8 || P.depth == 16); break;
    case 3: P.nch = 1;
        dok = (P.depth == 1 || P.depth == 2 || P.depth == 4 || P.depth == 8);
        break;
    case 4: P.nch = 2; dok = (P.depth == 8 || P.depth == 16); break;
    case 6: P.nch = 4; dok = (P.depth == 8 || P.depth == 16); break;
    default:
        um_set_error("PNG bad colour type");
        return 0;
    }
    if (!dok) { um_set_error("PNG bad bit depth"); return 0; }

    /* walk chunks to the first IDAT, collecting PLTE and tRNS */
    pos = off + 33;
    for (;;) {
        unsigned char ch[8];
        uint32_t len;
        if (um_read(pos, ch, 8) != 8) {
            um_set_error("PNG has no IDAT");
            return 0;
        }
        len = be32(ch);
        if (len > 0x7FFFFF00u) {
            um_set_error("PNG chunk length invalid");
            return 0;
        }
        if (!memcmp(ch + 4, "IDAT", 4)) { P.first_idat = pos; break; }
        if (!memcmp(ch + 4, "IEND", 4)) {
            um_set_error("PNG has no IDAT");
            return 0;
        }
        if (!memcmp(ch + 4, "PLTE", 4)) {
            unsigned char pb[768];
            long i;
            if (len == 0 || len > 768 || len % 3) {
                um_set_error("PNG bad PLTE");
                return 0;
            }
            if (um_read(pos + 8, pb, (long)len) != (long)len) {
                um_set_error("PNG truncated in PLTE");
                return 0;
            }
            P.pal_n = (int)(len / 3);
            for (i = 0; i < P.pal_n; i++)
                P.pal[i] = UM_PX(pb[i * 3], pb[i * 3 + 1], pb[i * 3 + 2], 255);
            seen_plte = 1;
        } else if (!memcmp(ch + 4, "tRNS", 4)) {
            unsigned char tb[256];
            if (P.ctype == 3) {             /* per-entry palette alpha      */
                long i;
                if (!seen_plte || len > (uint32_t)P.pal_n) {
                    um_set_error("PNG bad tRNS");
                    return 0;
                }
                if (um_read(pos + 8, tb, (long)len) != (long)len) {
                    um_set_error("PNG truncated in tRNS");
                    return 0;
                }
                for (i = 0; i < (long)len; i++)
                    P.pal[i] = (P.pal[i] & 0x00FFFFFFu) | ((um_px)tb[i] << 24);
                P.trns = 1;
            } else if (P.ctype == 0) {      /* grey colourkey               */
                if (len != 2 || um_read(pos + 8, tb, 2) != 2) {
                    um_set_error("PNG bad tRNS");
                    return 0;
                }
                P.kr = ((unsigned)tb[0] << 8) | tb[1];
                P.trns = 1;
            } else if (P.ctype == 2) {      /* RGB colourkey                */
                if (len != 6 || um_read(pos + 8, tb, 6) != 6) {
                    um_set_error("PNG bad tRNS");
                    return 0;
                }
                P.kr = ((unsigned)tb[0] << 8) | tb[1];
                P.kg = ((unsigned)tb[2] << 8) | tb[3];
                P.kb = ((unsigned)tb[4] << 8) | tb[5];
                P.trns = 1;
            }
            /* tRNS on types 4/6 is spec-invalid: ignored, not fatal */
        }
        /* every other chunk - ancillary or unknown - is skipped unread */
        pos += 12 + (long)len;
        if (pos <= 0) { um_set_error("PNG chunk length invalid"); return 0; }
    }
    if (P.ctype == 3 && !seen_plte) {
        um_set_error("PNG palette missing");
        return 0;
    }
    /* for depth < 16 the key was stored in the low bits; compare raw */

    P.rowmax = ((long)w * P.nch * P.depth + 7) / 8;     /* <= 128 KB        */
    P.fu = (P.nch * P.depth) / 8;
    if (P.fu < 1) P.fu = 1;
    P.cur  = (unsigned char *)um_alloc((unsigned long)P.rowmax);
    P.prev = (unsigned char *)um_alloc((unsigned long)P.rowmax);
    if (!P.cur || !P.prev) {
        png_close();
        um_set_error("PNG: out of memory");
        return 0;
    }

    P.base = off;
    P.w = (long)w;
    P.h = (long)h;
    P.open = 1;

    info->format = "PNG";
    info->w      = (int)w;
    info->h      = (int)h;
    info->bpp    = P.nch * P.depth;
    info->alpha  = (P.ctype == 4 || P.ctype == 6 || P.trns) ? 1 : 0;
    info->frames = 1;
    return 1;
}

static int png_frame(um_px *dst, int *delay_ms)
{
    int r;
    if (!P.open || !dst) { um_set_error("PNG decoder not open"); return -1; }
    if (delay_ms) *delay_ms = 0;            /* a still has no display time  */
    if (P.played) return 0;

    P.dst  = dst;
    P.done = 0;
    P.derr = 0;
    P.id_off = 0;
    P.id_rem = 0;
    P.id_next = P.first_idat;
    if (!pass_begin(0)) { um_set_error("PNG empty image"); return -1; }

    r = um_inflate(png_in, 0, png_out, 0, 1);
    P.dst = 0;
    if (P.derr) return -1;                  /* emit/unfilter set the error  */
    if (!P.done) {
        if (r) um_set_error("PNG image data ended early");
        return -1;                          /* !r: um_inflate said why      */
    }
    /* done + a sour inflate tail = every pixel arrived; accept the image */
    P.played = 1;
    return 1;
}

static void png_rewind(void)
{
    P.played = 0;                           /* frame() rebuilds all state   */
}

static int png_probe(const unsigned char *head, long n, const char *ext)
{
    (void)ext;
    return n >= 8 && !memcmp(head, "\x89PNG\r\n\x1a\n", 8);
}

static int png_open(um_image_info *info)
{
    return um_png_open_at(0, info);
}

const um_idecoder um_idec_png = {
    "png", png_probe, png_open, png_frame, png_rewind, png_close
};

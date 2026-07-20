/* ===========================================================================
 * unomedia - the BMP decoder, written from scratch against Microsoft's
 * BITMAPFILEHEADER / DIB documentation. Also the ICO payload core: the real
 * work lives in um_bmp_open_dib(), which decodes a bare DIB at any byte
 * offset, so the .bmp open() just reads the 14-byte file header and
 * delegates, and um_ico.c delegates with ico=1 (see unomedia_int.h).
 *
 * Handles what real files actually contain:
 *   - CORE (12) and INFO/V2/V3/OS22X/V4/V5 (40..124) DIB headers; unknown
 *     field tails are skipped, missing fields default to zero.
 *   - 1/4/8-bit palettes (CORE = 3-byte RGB triples, the rest 4-byte BGRX),
 *     16-bit (555 by default, BI_BITFIELDS masks honoured with arbitrary
 *     shifts and widths), 24-bit BGR, 32-bit BGRX/BGRA.
 *   - RLE8 and RLE4, including the EOL / EOB / delta escapes. Runs are
 *     clamped to the frame rect no matter what the stream claims; pixels an
 *     escape skips over are left as palette entry 0 (the spec calls them
 *     undefined).
 *   - Bottom-up and top-down rows; ICO mode (biHeight halved, XOR + AND
 *     planes, AND mask as 1-bit transparency for depths < 32).
 *
 * The 32-bit alpha heuristic (the classic one): the fourth byte is treated
 * as alpha only if some pixel has A != 0. A file whose alpha channel is all
 * zero was written by a BGRX encoder that never touched the byte, so it is
 * decoded fully opaque - except in ICO mode, where the all-zero case falls
 * back to the AND mask, which is what every icon renderer since Windows 3.x
 * has done.
 *
 * Pixels stream through a small buffered reader over um_read - nothing but
 * the ~2.5 KB decode state (um_alloc'd) is resident, whatever the file size.
 * BI_JPEG / BI_PNG embedded streams are recognised and refused by name.
 * ======================================================================== */
#include "unomedia.h"
#include "unomedia_int.h"
#include <string.h>
#include <stdint.h>

#define BI_RGB        0
#define BI_RLE8       1
#define BI_RLE4       2
#define BI_BITFIELDS  3
#define BI_JPEG       4
#define BI_PNG        5
#define BI_ALPHAFLD   6         /* BI_ALPHABITFIELDS (Windows CE lineage)  */

#define BR_BUF 512

typedef struct {
    int  w, h;                  /* logical size, h always positive          */
    int  bpp, comp;
    int  topdown, ico;
    int  saw_alpha;             /* any decoded A != 0 (masks path)          */
    uint32_t mr, mg, mb, ma;    /* channel masks for the 16/32-bit paths    */
    um_px pal[256];
    long pixoff, end;           /* pixel bytes live in [pixoff, end)        */
    long andoff;                /* ICO AND mask offset, <0 = none           */
    int  done;                  /* the single still frame was delivered     */
    /* sequential buffered reader over [_, end) */
    long boff;                  /* next file offset to refill from          */
    int  blen, bpos;
    unsigned char buf[BR_BUF];
} bmp_st;

static bmp_st *st;
static long    g_offbits;       /* bfOffBits hint from the plain-file open  */

static uint32_t rd32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

/* ---- the buffered byte reader --------------------------------------------- */
static void br_seek(long off)
{ st->boff = off; st->blen = 0; st->bpos = 0; }

static int br_byte(void)                          /* next byte, -1 = end   */
{
    if (st->bpos >= st->blen) {
        long want = st->end - st->boff, got;
        if (want <= 0) return -1;
        if (want > BR_BUF) want = BR_BUF;
        got = um_read(st->boff, st->buf, want);
        if (got <= 0) return -1;
        st->boff += got;
        st->blen = (int)got;
        st->bpos = 0;
    }
    return st->buf[st->bpos++];
}

static int br_need(unsigned char *d, int n)       /* n bytes or fail       */
{
    int i, c;
    for (i = 0; i < n; i++) {
        if ((c = br_byte()) < 0) return 0;
        d[i] = (unsigned char)c;
    }
    return 1;
}

/* ---- mask channel extraction ----------------------------------------------
 * Pull the channel `mask` selects out of `pix` and scale it to 8 bits with
 * rounding. Works for any contiguous mask at any shift; wider-than-8-bit
 * channels are folded down first so the scale never overflows. */
static unsigned mask_ch(uint32_t pix, uint32_t mask)
{
    uint32_t v, top;
    int sh = 0;
    if (!mask) return 0;
    while (!(mask & 1)) { mask >>= 1; sh++; }
    v   = (pix >> sh) & mask;
    top = mask;
    while (top > 255) { top >>= 1; v >>= 1; }
    return (unsigned)((v * 255u + top / 2u) / top);
}

static um_px px_masks(uint32_t pix)
{
    unsigned a = st->ma ? mask_ch(pix, st->ma) : 255u;
    if (a) st->saw_alpha = 1;
    return UM_PX(mask_ch(pix, st->mr), mask_ch(pix, st->mg),
                 mask_ch(pix, st->mb), a);
}

/* ---- uncompressed rows ---------------------------------------------------- */
static int raw_decode(um_px *dst)
{
    long stride = (((long)st->w * st->bpp + 31) / 32) * 4, used, r;
    unsigned char t[4];
    int x;

    br_seek(st->pixoff);
    for (r = 0; r < st->h; r++) {
        int y = st->topdown ? (int)r : st->h - 1 - (int)r;
        um_px *row = dst + (long)y * st->w;
        used = 0;
        switch (st->bpp) {
        case 1: case 4: case 8: {
            int acc = 0, bits = 0;
            for (x = 0; x < st->w; x++) {
                if (bits == 0) {
                    if (!br_need(t, 1)) goto trunc;
                    acc = t[0]; bits = 8; used++;
                }
                bits -= st->bpp;
                row[x] = st->pal[(acc >> bits) & ((1 << st->bpp) - 1)];
            }
            break;
        }
        case 16:
            for (x = 0; x < st->w; x++) {
                if (!br_need(t, 2)) goto trunc;
                row[x] = px_masks(rd16(t));
            }
            used = (long)st->w * 2;
            break;
        case 24:
            for (x = 0; x < st->w; x++) {
                if (!br_need(t, 3)) goto trunc;
                row[x] = UM_PX(t[2], t[1], t[0], 255);
            }
            used = (long)st->w * 3;
            break;
        default: /* 32 */
            for (x = 0; x < st->w; x++) {
                if (!br_need(t, 4)) goto trunc;
                row[x] = px_masks(rd32(t));
            }
            used = (long)st->w * 4;
            break;
        }
        while (used++ < stride)
            if (br_byte() < 0) goto trunc;
    }
    return 1;
trunc:
    um_set_error("truncated BMP pixel data");
    return 0;
}

/* ---- RLE8 / RLE4 ----------------------------------------------------------
 * `row` counts stored rows (bottom-up unless topdown); every write is
 * bounds-checked, and the cursors are clamped after each opcode so no
 * adversarial run of deltas can overflow them. */
static void rle_put(um_px *dst, int x, int row, int idx)
{
    int y;
    if (x < 0 || x >= st->w || row < 0 || row >= st->h) return;
    y = st->topdown ? row : st->h - 1 - row;
    dst[(long)y * st->w + x] = st->pal[idx & 255];
}

static int rle_decode(um_px *dst)
{
    int x = 0, row = 0, four = (st->comp == BI_RLE4);
    long i;

    for (i = 0; i < (long)st->w * st->h; i++) dst[i] = st->pal[0];

    br_seek(st->pixoff);
    for (;;) {
        int c = br_byte(), v = br_byte();
        if (c < 0 || v < 0) goto trunc;
        if (c > 0) {                              /* encoded run           */
            for (i = 0; i < c; i++, x++)
                rle_put(dst, x, row,
                        four ? ((i & 1) ? (v & 15) : (v >> 4)) : v);
        } else if (v == 0) {                      /* end of line           */
            x = 0; row++;
        } else if (v == 1) {                      /* end of bitmap         */
            break;
        } else if (v == 2) {                      /* delta                 */
            int dx = br_byte(), dy = br_byte();
            if (dx < 0 || dy < 0) goto trunc;
            x += dx; row += dy;
        } else {                                  /* literal (absolute) run */
            int nb = four ? (v + 1) / 2 : v, b = 0;
            for (i = 0; i < v; i++, x++) {
                if (!four || !(i & 1)) {
                    if ((b = br_byte()) < 0) goto trunc;
                }
                rle_put(dst, x, row,
                        four ? ((i & 1) ? (b & 15) : (b >> 4)) : b);
            }
            if (nb & 1)                           /* pad to a word         */
                if (br_byte() < 0) goto trunc;
        }
        if (x > st->w) x = st->w;                 /* keep cursors bounded  */
        if (row > st->h) row = st->h;
    }
    return 1;
trunc:
    um_set_error("truncated BMP RLE data");
    return 0;
}

/* ---- the ICO AND mask: 1 bit per pixel, MSB first, rows padded to 32 bits,
 * stored bottom-up after the XOR plane. Bit set = transparent. */
static int and_apply(um_px *dst)
{
    long astride = (((long)st->w + 31) / 32) * 4, used, r;
    int x, acc = 0, bits;
    unsigned char t[1];

    br_seek(st->andoff);
    for (r = 0; r < st->h; r++) {
        um_px *row = dst + (long)(st->h - 1 - r) * st->w;
        used = 0; bits = 0;
        for (x = 0; x < st->w; x++) {
            if (bits == 0) {
                if (!br_need(t, 1)) goto trunc;
                acc = t[0]; bits = 8; used++;
            }
            bits--;
            row[x] = ((acc >> bits) & 1) ? (row[x] & 0x00FFFFFFu)
                                         : (row[x] | 0xFF000000u);
        }
        while (used++ < astride)
            if (br_byte() < 0) goto trunc;
    }
    return 1;
trunc:
    um_set_error("truncated ICO AND mask");
    return 0;
}

/* ---- vtable --------------------------------------------------------------- */
static void bmp_close(void)
{
    um_free(st);
    st = 0;
}

static int bmp_frame(um_px *dst, int *delay_ms)
{
    if (!st) { um_set_error("BMP decoder not open"); return -1; }
    if (st->done) return 0;
    *delay_ms = 0;
    st->saw_alpha = 0;

    if (st->comp == BI_RLE8 || st->comp == BI_RLE4) {
        if (!rle_decode(dst)) return -1;
    } else {
        if (!raw_decode(dst)) return -1;
    }

    if (st->ico && st->andoff >= 0 && (!st->ma || !st->saw_alpha)) {
        if (!and_apply(dst)) return -1;   /* AND mask decides transparency */
    } else if (st->ma && !st->saw_alpha) {
        long i, n = (long)st->w * st->h;  /* BGRX in disguise: force opaque */
        for (i = 0; i < n; i++) dst[i] |= 0xFF000000u;
    }
    st->done = 1;
    return 1;
}

static void bmp_rewind(void)
{
    if (st) st->done = 0;
}

/* ---- the DIB core (also the ICO payload entry, see unomedia_int.h) -------- */
int um_bmp_open_dib(long off, long end, int ico, um_image_info *info)
{
    unsigned char hd[124], pt[1024];
    uint32_t hs, comp = BI_RGB, clr = 0;
    uint32_t mr = 0, mg = 0, mb = 0, ma = 0;
    long w, hraw, h, palloff, pixoff, stride, andoff = -1;
    long hint = g_offbits;
    int  bpp, topdown = 0, palsz, paln, loadn, i, hb;

    bmp_close();
    g_offbits = 0;
    if (end <= 0 || end > um_size()) end = um_size();

    memset(hd, 0, sizeof hd);
    if (um_read(off, hd, 4) != 4)
        { um_set_error("truncated BMP header"); return 0; }
    hs = rd32(hd);
    if (hs != 12 && (hs < 16 || hs > 1024))
        { um_set_error("unrecognised BMP header size"); return 0; }
    hb = hs > 124 ? 124 : (int)hs;
    if (um_read(off, hd, hb) != hb)
        { um_set_error("truncated BMP header"); return 0; }

    if (hs == 12) {                               /* BITMAPCOREHEADER      */
        w    = rd16(hd + 4);
        hraw = rd16(hd + 6);
        bpp  = rd16(hd + 10);
        palsz = 3;
    } else {                                      /* INFO and successors   */
        w    = (int32_t)rd32(hd + 4);
        hraw = (int32_t)rd32(hd + 8);
        bpp  = rd16(hd + 14);
        comp = rd32(hd + 16);
        clr  = rd32(hd + 32);
        palsz = 4;
    }

    if (hraw < 0) { topdown = 1; hraw = -hraw; }
    if (ico) {
        if (topdown)
            { um_set_error("top-down ICO bitmap"); return 0; }
        if (hraw <= 0 || (hraw & 1))
            { um_set_error("bad ICO bitmap height"); return 0; }
        h = hraw / 2;                             /* XOR + AND planes      */
    } else {
        h = hraw;
    }
    if (w <= 0 || h <= 0 || w > UM_MAX_DIM || h > UM_MAX_DIM ||
        w * h > UM_MAX_PIXELS)
        { um_set_error("BMP dimensions out of range"); return 0; }

    if (bpp != 1 && bpp != 4 && bpp != 8 &&
        bpp != 16 && bpp != 24 && bpp != 32)
        { um_set_error("unsupported BMP bit depth"); return 0; }

    switch (comp) {
    case BI_RGB:
        break;
    case BI_RLE8:
        if (bpp != 8) { um_set_error("RLE8 BMP must be 8-bit"); return 0; }
        break;
    case BI_RLE4:
        if (bpp != 4) { um_set_error("RLE4 BMP must be 4-bit"); return 0; }
        break;
    case BI_BITFIELDS:
    case BI_ALPHAFLD:
        if (bpp != 16 && bpp != 32)
            { um_set_error("BMP bitfields need 16/32-bit pixels"); return 0; }
        break;
    case BI_JPEG:
        um_set_error("JPEG-compressed BMP - not decoded in this build");
        return 0;
    case BI_PNG:
        um_set_error("PNG-compressed BMP - not decoded in this build");
        return 0;
    default:
        um_set_error("unknown BMP compression");
        return 0;
    }
    if (ico && (comp == BI_RLE8 || comp == BI_RLE4))
        { um_set_error("compressed ICO bitmap"); return 0; }

    /* channel masks: defaults, then whatever the header/stream provides */
    if (bpp == 16) { mr = 0x7C00; mg = 0x03E0; mb = 0x001F; }
    if (bpp == 32) { mr = 0xFF0000; mg = 0xFF00; mb = 0xFF; ma = 0xFF000000u; }
    palloff = off + (long)hs;
    if (comp == BI_BITFIELDS || comp == BI_ALPHAFLD) {
        if (hs >= 52) {                           /* masks inside the header */
            mr = rd32(hd + 40); mg = rd32(hd + 44); mb = rd32(hd + 48);
            ma = hs >= 56 ? rd32(hd + 52) : 0;
        } else {                                  /* masks follow it        */
            int nm = (comp == BI_ALPHAFLD) ? 16 : 12;
            unsigned char m[16];
            if (um_read(palloff, m, nm) != nm)
                { um_set_error("truncated BMP bitfield masks"); return 0; }
            mr = rd32(m); mg = rd32(m + 4); mb = rd32(m + 8);
            ma = nm == 16 ? rd32(m + 12) : 0;
            palloff += nm;
        }
        if (bpp == 16) { mr &= 0xFFFF; mg &= 0xFFFF; mb &= 0xFFFF; ma &= 0xFFFF; }
        if (!mr && !mg && !mb)
            { um_set_error("empty BMP bitfield masks"); return 0; }
    }

    /* palette: sized by biClrUsed (0 = full for indexed depths). Truecolour
       files may still carry an optimisation palette we only skip over. */
    paln = 0;
    if (bpp <= 8) paln = clr ? (int)clr : (1 << bpp);
    else if (clr) paln = (int)clr;
    if (clr > 4096) { um_set_error("implausible BMP palette size"); return 0; }
    pixoff = palloff + (long)paln * palsz;
    if (hint >= pixoff && hint < end && !ico)
        pixoff = hint;                            /* honour bfOffBits gaps */

    /* the decode state */
    st = (bmp_st *)um_alloc(sizeof *st);
    if (!st) { um_set_error("out of memory"); return 0; }
    memset(st, 0, sizeof *st);
    for (i = 0; i < 256; i++) st->pal[i] = UM_PX(0, 0, 0, 255);
    loadn = (bpp <= 8) ? (paln > 256 ? 256 : paln) : 0;
    if (loadn) {
        if (um_read(palloff, pt, loadn * palsz) != loadn * palsz)
            { um_set_error("truncated BMP palette"); bmp_close(); return 0; }
        for (i = 0; i < loadn; i++) {
            const unsigned char *p = pt + i * palsz;
            st->pal[i] = UM_PX(p[2], p[1], p[0], 255);
        }
    }

    /* size sanity before believing anything (compare by subtraction - the
       products stay under 2^30 thanks to the dimension guards above) */
    stride = ((w * bpp + 31) / 32) * 4;
    if (pixoff < 0 || pixoff >= end)
        { um_set_error("BMP pixel data outside the file"); bmp_close(); return 0; }
    if (comp != BI_RLE8 && comp != BI_RLE4 && stride * h > end - pixoff)
        { um_set_error("truncated BMP pixel data"); bmp_close(); return 0; }
    if (ico) {
        long astride = ((w + 31) / 32) * 4;
        andoff = pixoff + stride * h;
        if (comp != BI_RGB && comp != BI_BITFIELDS && comp != BI_ALPHAFLD)
            andoff = -1;
        else if (astride * h > end - andoff)
            { um_set_error("truncated ICO AND mask"); bmp_close(); return 0; }
    }

    st->w = (int)w;  st->h = (int)h;
    st->bpp = bpp;   st->comp = (int)comp;
    st->topdown = topdown;  st->ico = ico;
    st->mr = mr; st->mg = mg; st->mb = mb; st->ma = ma;
    st->pixoff = pixoff;  st->end = end;  st->andoff = andoff;

    info->format = "BMP";
    info->w = (int)w;  info->h = (int)h;
    info->bpp = bpp;
    info->alpha = (ma != 0 || ico) ? 1 : 0;
    info->frames = 1;
    return 1;
}

/* ---- the plain .bmp file entry -------------------------------------------- */
static int bmp_probe(const unsigned char *head, long n, const char *ext)
{
    (void)ext;
    return n >= 14 && head[0] == 'B' && head[1] == 'M';
}

static int bmp_open(um_image_info *info)
{
    unsigned char fh[14];
    int r;
    if (um_read(0, fh, 14) != 14)
        { um_set_error("truncated BMP file header"); return 0; }
    g_offbits = (long)(rd32(fh + 10) & 0x7FFFFFFFu);
    r = um_bmp_open_dib(14, 0, 0, info);
    g_offbits = 0;
    return r;
}

const um_idecoder um_idec_bmp =
    { "bmp", bmp_probe, bmp_open, bmp_frame, bmp_rewind, bmp_close };

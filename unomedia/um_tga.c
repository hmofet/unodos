/* ===========================================================================
 * unomedia - the Truevision TGA decoder, written from scratch against the
 * TGA 2.0 specification.
 *
 * TGA is the one roster format with NO magic number, so the probe demands
 * both the "TGA" extension and a plausible 18-byte header (colormap type
 * 0/1, image type 1/2/3/9/10/11, pixel depth 8/15/16/24/32, nonzero
 * dimensions) before claiming the file. Handled: colormapped images (8- or
 * 16-bit indices into 15/16/24/32-bit palette entries), truecolor at
 * 15/16/24/32 bits, 8-bit grayscale, each in both uncompressed and RLE
 * (types 9/10/11) form; the image-descriptor origin bits, including the
 * common bottom-up layout and the rare right-to-left flip; the ID field is
 * skipped and the optional TGA 2.0 footer/extension area is ignored (it
 * never affects the pixel data).
 *
 * Simplifications, stated: 15- and 16-bit pixels are treated as opaque
 * RGB555 - the attribute bit is ignored rather than guessed at, because
 * real 16-bit files disagree about what it means. 24-bit is BGR, 32-bit is
 * BGRA with alpha honoured. Grayscale is 8-bit only.
 *
 * Distrust: dimensions are guarded before anything is allocated, colormap
 * indices are range-checked against the palette, and RLE packet counts are
 * clamped to the pixels that remain - a lying stream can end the image
 * early (truncation error) but never write outside it.
 * ======================================================================== */
#include "unomedia.h"
#include <string.h>
#include <stdint.h>

/* ---- buffered forward reader over um_read ------------------------------- */
#define TGA_BUF 512
static struct {
    long pos;                       /* file offset of the byte after buf[n]  */
    int  n, at;
    unsigned char buf[TGA_BUF];
} t_rd;

static void t_seek(long off) { t_rd.pos = off; t_rd.n = 0; t_rd.at = 0; }

static int t_byte(void)
{
    if (t_rd.at >= t_rd.n) {
        long got = um_read(t_rd.pos, t_rd.buf, TGA_BUF);
        if (got <= 0) return -1;
        t_rd.pos += got;
        t_rd.n = (int)got;
        t_rd.at = 0;
    }
    return t_rd.buf[t_rd.at++];
}

static int t_bytes(unsigned char *dst, int n)
{
    int i, c;
    for (i = 0; i < n; i++) {
        c = t_byte();
        if (c < 0) return 0;
        dst[i] = (unsigned char)c;
    }
    return 1;
}

/* ---- decoder state ------------------------------------------------------- */
static int    t_type;               /* header image type (1..3, 9..11)       */
static int    t_depth, t_bppx;      /* pixel bits / bytes on disk            */
static int    t_w, t_h;
static int    t_hflip, t_topdown;   /* descriptor origin bits                */
static long   t_data_off;           /* first byte of pixel data              */
static int    t_cm_first, t_cm_len; /* colormap window                       */
static um_px *t_pal;                /* converted palette (um_alloc)          */
static int    t_done;

static int le16(const unsigned char *p)
{ return (int)p[0] | ((int)p[1] << 8); }

/* one stored color at `bits` depth -> um_px. 15/16-bit is opaque RGB555
 * (5-bit channels widened with the classic (v<<3)|(v>>2)); 24 BGR; 32 BGRA. */
static um_px t_color(const unsigned char *p, int bits)
{
    if (bits == 15 || bits == 16) {
        unsigned v = (unsigned)le16(p);
        unsigned r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return UM_PX((r << 3) | (r >> 2), (g << 3) | (g >> 2),
                     (b << 3) | (b >> 2), 255);
    }
    if (bits == 24)
        return UM_PX(p[2], p[1], p[0], 255);
    return UM_PX(p[2], p[1], p[0], p[3]);
}

/* one raw pixel (t_bppx bytes) -> um_px; 0 = error (um_set_error done) */
static int t_pixel(const unsigned char *p, um_px *out)
{
    int base = t_type & 7;
    if (base == 3) {                                 /* grayscale            */
        *out = UM_PX(p[0], p[0], p[0], 255);
        return 1;
    }
    if (base == 1) {                                 /* colormapped          */
        long idx = (t_bppx == 2) ? le16(p) : p[0];
        idx -= t_cm_first;
        if (idx < 0 || idx >= t_cm_len) {
            um_set_error("TGA colormap index out of range");
            return 0;
        }
        *out = t_pal[idx];
        return 1;
    }
    *out = t_color(p, t_depth);                      /* truecolor            */
    return 1;
}

static int tga_probe(const unsigned char *h, long n, const char *ext)
{
    int cm, ty, dep;
    if (strcmp(ext, "TGA") != 0) return 0;           /* no magic: ext gates  */
    if (n < 18) return 0;
    cm = h[1]; ty = h[2]; dep = h[16];
    if (cm > 1) return 0;
    if (ty != 1 && ty != 2 && ty != 3 && ty != 9 && ty != 10 && ty != 11)
        return 0;
    if (dep != 8 && dep != 15 && dep != 16 && dep != 24 && dep != 32)
        return 0;
    if (le16(h + 12) == 0 || le16(h + 14) == 0) return 0;
    return 1;
}

static void tga_close(void)
{
    um_free(t_pal);
    t_pal = 0;
    t_done = 0;
}

static int tga_open(um_image_info *info)
{
    unsigned char h[18];
    int cm_type, cm_bits, idlen, base, desc;
    long cm_bytes = 0;

    t_pal = 0; t_done = 0;
    t_seek(0);
    if (!t_bytes(h, 18)) { um_set_error("truncated TGA header"); return 0; }

    idlen      = h[0];
    cm_type    = h[1];
    t_type     = h[2];
    t_cm_first = le16(h + 3);
    t_cm_len   = le16(h + 5);
    cm_bits    = h[7];
    t_w        = le16(h + 12);
    t_h        = le16(h + 14);
    t_depth    = h[16];
    desc       = h[17];
    t_bppx     = (t_depth + 7) / 8;
    t_hflip    = (desc >> 4) & 1;
    t_topdown  = (desc >> 5) & 1;
    base       = t_type & 7;

    if (t_w <= 0 || t_h <= 0 || t_w > UM_MAX_DIM || t_h > UM_MAX_DIM ||
        (long)t_w * t_h > UM_MAX_PIXELS) {
        um_set_error("TGA dimensions out of range");
        return 0;
    }
    /* type/depth consistency - a header that lies about one of these would
       misparse every byte that follows, so refuse it precisely */
    if (base == 1 && (cm_type != 1 || (t_depth != 8 && t_depth != 16))) {
        um_set_error("colormapped TGA needs a colormap and 8/16-bit indices");
        return 0;
    }
    if (base == 2 && t_depth == 8) {
        um_set_error("truecolor TGA cannot be 8-bit");
        return 0;
    }
    if (base == 3 && t_depth != 8) {
        um_set_error("grayscale TGA depth must be 8");
        return 0;
    }

    if (cm_type == 1) {
        if (cm_bits != 15 && cm_bits != 16 && cm_bits != 24 && cm_bits != 32) {
            um_set_error("TGA colormap entry size not 15/16/24/32");
            return 0;
        }
        if (t_cm_len <= 0) {
            um_set_error("TGA colormap declared but empty");
            return 0;
        }
        cm_bytes = (long)t_cm_len * ((cm_bits + 7) / 8);
    }

    if (base == 1) {                     /* load + convert the palette       */
        int e, ebytes = (cm_bits + 7) / 8;
        unsigned char ent[4];
        t_pal = (um_px *)um_alloc((unsigned long)t_cm_len * sizeof(um_px));
        if (!t_pal) { um_set_error("out of memory"); return 0; }
        t_seek(18 + idlen);
        for (e = 0; e < t_cm_len; e++) {
            if (!t_bytes(ent, ebytes)) {
                um_set_error("truncated TGA colormap");
                tga_close();
                return 0;
            }
            t_pal[e] = t_color(ent, cm_bits);
        }
    }

    t_data_off = 18 + idlen + cm_bytes;

    info->format = "TGA";
    info->w      = t_w;
    info->h      = t_h;
    info->bpp    = t_depth;
    info->alpha  = (base == 2 && t_depth == 32) ||
                   (base == 1 && cm_bits == 32);
    info->frames = 1;
    return 1;
}

static int tga_frame(um_px *dst, int *delay_ms)
{
    long total = (long)t_w * t_h, i;
    long run = 0, raw = 0;
    um_px runpx = 0;
    unsigned char p[4];
    int rle = t_type >= 9;

    if (t_done) return 0;
    t_seek(t_data_off);

    for (i = 0; i < total; i++) {
        um_px px;
        if (rle) {
            if (run == 0 && raw == 0) {
                int c = t_byte();
                if (c < 0) {
                    um_set_error("truncated TGA RLE stream");
                    return -1;
                }
                if (c & 0x80) {
                    run = (c & 0x7F) + 1;
                    if (!t_bytes(p, t_bppx)) {
                        um_set_error("truncated TGA RLE stream");
                        return -1;
                    }
                    if (!t_pixel(p, &runpx)) return -1;
                } else {
                    raw = (c & 0x7F) + 1;
                }
                /* clamp to the pixels that remain: the image bounds win
                   over whatever the packet header claims */
                if (run > total - i) run = total - i;
                if (raw > total - i) raw = total - i;
            }
            if (run > 0) {
                px = runpx;
                run--;
            } else {
                if (!t_bytes(p, t_bppx)) {
                    um_set_error("truncated TGA pixel data");
                    return -1;
                }
                if (!t_pixel(p, &px)) return -1;
                raw--;
            }
        } else {
            if (!t_bytes(p, t_bppx)) {
                um_set_error("truncated TGA pixel data");
                return -1;
            }
            if (!t_pixel(p, &px)) return -1;
        }
        {   /* file order -> screen order via the descriptor origin bits */
            long fx = i % t_w, fy = i / t_w;
            long dx = t_hflip ? (t_w - 1 - fx) : fx;
            long dy = t_topdown ? fy : (t_h - 1 - fy);
            dst[dy * t_w + dx] = px;
        }
    }
    t_done = 1;
    if (delay_ms) *delay_ms = 0;
    return 1;
}

static void tga_rewind(void) { t_done = 0; }

const um_idecoder um_idec_tga =
    { "TGA", tga_probe, tga_open, tga_frame, tga_rewind, tga_close };

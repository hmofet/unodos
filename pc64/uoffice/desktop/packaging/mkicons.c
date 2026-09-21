/* ===========================================================================
 * mkicons.c - the desktop app icons, rendered from the pc64 shell's own
 * emblems (pc64_icons.c: PCI_UOWORD / PCI_UOCALC / PCI_UOSHOW).
 *
 * The suite already has icons: the ones the UnoDOS launcher and taskbar draw.
 * They are procedural (a 32-unit grid scaled to any size), so rather than
 * drawing a second set, this renders those at every size a desktop OS asks
 * for and writes the three container formats:
 *
 *   <out>/<name>-<N>.png   N = 16 32 48 64 128 256 512   (Linux hicolor)
 *   <out>/<name>.ico       16..256, PNG-compressed entries   (Windows)
 *   <out>/<name>.icns      16..1024, PNG entries             (macOS)
 *
 * Each size is drawn 4x (or as large as the framebuffer allows) and
 * box-filtered down, which turns the emblems' 1-pixel stair-steps into
 * anti-aliased diagonals.  The background is a key colour that becomes
 * alpha 0.
 *
 * The outputs are committed (packaging/icons/); rerun when the emblems move:
 *   cmake --build build --target uodesk_mkicons && build/uodesk_mkicons packaging/icons
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "fb.h"
#include "pc64_icons.h"

int uno_fb_w = FB_MAX_W, uno_fb_h = FB_MAX_H;
/* pc64_icons.c recolours to the live shell theme; NULL = full-colour art */
const struct unoui_theme *pc64_shell_theme(void) { return 0; }

#define KEY FB_RGB(255, 0, 255)

typedef struct { unsigned char *d; size_t n, cap; } buf;
static void put(buf *b, const void *p, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2;
        b->d = (unsigned char *)realloc(b->d, b->cap);
        if (!b->d) { perror("realloc"); exit(1); }
    }
    memcpy(b->d + b->n, p, n); b->n += n;
}
static void be32(buf *b, unsigned v)
{ unsigned char c[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16),
                         (unsigned char)(v >> 8), (unsigned char)v }; put(b, c, 4); }
static void le32(buf *b, unsigned v)
{ unsigned char c[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                         (unsigned char)(v >> 16), (unsigned char)(v >> 24) }; put(b, c, 4); }
static void le16(buf *b, unsigned v)
{ unsigned char c[2] = { (unsigned char)v, (unsigned char)(v >> 8) }; put(b, c, 2); }

/* ---- a minimal PNG writer: RGBA8, one zlib stream ------------------- */
static unsigned crc_tab[256];
static unsigned crc(const unsigned char *p, size_t n, unsigned c)
{
    size_t i;
    if (!crc_tab[1]) {
        unsigned k, j;
        for (k = 0; k < 256; k++) {
            unsigned x = k;
            for (j = 0; j < 8; j++) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
            crc_tab[k] = x;
        }
    }
    c = ~c;
    for (i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void chunk(buf *b, const char *type, const unsigned char *p, size_t n)
{
    unsigned c;
    be32(b, (unsigned)n);
    put(b, type, 4);
    if (n) put(b, p, n);
    c = crc((const unsigned char *)type, 4, 0);
    c = crc(p, n, c);
    be32(b, c);
}
static void png(buf *out, const unsigned char *rgba, int w, int h)
{
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    buf raw = { 0, 0, 0 }, z = { 0, 0, 0 };
    unsigned char ihdr[13];
    uLongf zn;
    int y;
    for (y = 0; y < h; y++) {                      /* filter 0 per row */
        unsigned char f = 0;
        put(&raw, &f, 1);
        put(&raw, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    zn = compressBound((uLong)raw.n);
    z.d = (unsigned char *)malloc(zn); z.cap = zn;
    if (!z.d || compress2(z.d, &zn, raw.d, (uLong)raw.n, 9) != Z_OK) {
        fprintf(stderr, "deflate failed\n");
        exit(1);
    }
    z.n = zn;
    put(out, sig, 8);
    ihdr[0] = (unsigned char)(w >> 24); ihdr[1] = (unsigned char)(w >> 16);
    ihdr[2] = (unsigned char)(w >> 8);  ihdr[3] = (unsigned char)w;
    ihdr[4] = (unsigned char)(h >> 24); ihdr[5] = (unsigned char)(h >> 16);
    ihdr[6] = (unsigned char)(h >> 8);  ihdr[7] = (unsigned char)h;
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(out, "IHDR", ihdr, 13);
    chunk(out, "IDAT", z.d, z.n);
    chunk(out, "IEND", 0, 0);
    free(raw.d); free(z.d);
}

/* ---- render one size, supersampled --------------------------------------- */
static unsigned char *render(int icon, int size)
{
    int ss = 4, x, y, i, j;
    unsigned char *o;
    unoui_rect box;
    while (ss > 1 && size * ss > FB_MAX_H) ss--;
    uno_fb_w = size * ss; uno_fb_h = size * ss;
    fb_reset_clip();
    fb_fill_rect(0, 0, uno_fb_w, uno_fb_h, KEY);
    box.x = 0; box.y = 0; box.w = size * ss; box.h = size * ss;
    pc64_icon_emblem(icon, box);
    o = (unsigned char *)malloc((size_t)size * size * 4);
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++) {
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (j = 0; j < ss; j++)
                for (i = 0; i < ss; i++) {
                    fb_px p = fb[(y * ss + j) * uno_fb_w + x * ss + i];
                    if (p == KEY) continue;
                    r += p & 0xFF; g += (p >> 8) & 0xFF; b += (p >> 16) & 0xFF; n++;
                }
            {
                unsigned char *q = o + ((size_t)y * size + x) * 4;
                q[0] = (unsigned char)(n ? r / n : 0);
                q[1] = (unsigned char)(n ? g / n : 0);
                q[2] = (unsigned char)(n ? b / n : 0);
                q[3] = (unsigned char)(n * 255 / (unsigned)(ss * ss));
            }
        }
    return o;
}

static void write_file(const char *path, const buf *b)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b->d, 1, b->n, f) != b->n || fclose(f) != 0) { perror(path); exit(1); }
}

int main(int argc, char **argv)
{
    static const struct { const char *name; int id; } apps[] = {
        { "unoword", PCI_UOWORD }, { "unocalc", PCI_UOCALC }, { "unoshow", PCI_UOSHOW }
    };
    static const int pngsz[] = { 16, 32, 48, 64, 128, 256, 512 };
    static const int icosz[] = { 16, 24, 32, 48, 64, 128, 256 };
    /* icns: OSType per PNG size (icp4/5/6 = 16/32/64, ic07..ic10 = 128..1024) */
    static const struct { const char *t; int sz; } icns[] = {
        { "icp4", 16 }, { "icp5", 32 }, { "icp6", 64 }, { "ic07", 128 },
        { "ic08", 256 }, { "ic09", 512 }, { "ic10", 1024 }
    };
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    unsigned a, k;

    for (a = 0; a < sizeof apps / sizeof apps[0]; a++) {
        buf ico = { 0, 0, 0 }, dirb = { 0, 0, 0 }, data = { 0, 0, 0 }, ic = { 0, 0, 0 }, body = { 0, 0, 0 };
        unsigned nico = sizeof icosz / sizeof icosz[0];

        for (k = 0; k < sizeof pngsz / sizeof pngsz[0]; k++) {
            buf p = { 0, 0, 0 };
            unsigned char *px = render(apps[a].id, pngsz[k]);
            png(&p, px, pngsz[k], pngsz[k]);
            snprintf(path, sizeof path, "%s/%s-%d.png", dir, apps[a].name, pngsz[k]);
            write_file(path, &p);
            free(px); free(p.d);
        }

        /* .ico: ICONDIR, one ICONDIRENTRY per size, then the PNGs */
        le16(&ico, 0); le16(&ico, 1); le16(&ico, nico);
        for (k = 0; k < nico; k++) {
            buf p = { 0, 0, 0 };
            unsigned char *px = render(apps[a].id, icosz[k]);
            unsigned char wh = (unsigned char)(icosz[k] >= 256 ? 0 : icosz[k]);
            png(&p, px, icosz[k], icosz[k]);
            put(&dirb, &wh, 1); put(&dirb, &wh, 1);
            { unsigned char z2[2] = { 0, 0 }; put(&dirb, z2, 2); }  /* colours, reserved */
            le16(&dirb, 1); le16(&dirb, 32);
            le32(&dirb, (unsigned)p.n);
            le32(&dirb, 6 + 16 * nico + (unsigned)data.n);
            put(&data, p.d, p.n);
            free(px); free(p.d);
        }
        put(&ico, dirb.d, dirb.n); put(&ico, data.d, data.n);
        snprintf(path, sizeof path, "%s/%s.ico", dir, apps[a].name);
        write_file(path, &ico);

        /* .icns: 'icns' + total length, then (type, length, PNG) records */
        for (k = 0; k < sizeof icns / sizeof icns[0]; k++) {
            buf p = { 0, 0, 0 };
            unsigned char *px = render(apps[a].id, icns[k].sz);
            png(&p, px, icns[k].sz, icns[k].sz);
            put(&body, icns[k].t, 4); be32(&body, 8 + (unsigned)p.n); put(&body, p.d, p.n);
            free(px); free(p.d);
        }
        put(&ic, "icns", 4); be32(&ic, 8 + (unsigned)body.n); put(&ic, body.d, body.n);
        snprintf(path, sizeof path, "%s/%s.icns", dir, apps[a].name);
        write_file(path, &ic);

        free(ico.d); free(dirb.d); free(data.d); free(ic.d); free(body.d);
        printf("%s: png x%u, ico, icns\n", apps[a].name, (unsigned)(sizeof pngsz / sizeof pngsz[0]));
    }
    return 0;
}

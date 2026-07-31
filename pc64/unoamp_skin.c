/* UnoAmp skin engine: Winamp 2 .wsz skins.
 *
 * Phase 3 of docs/PLAYER-WINAMP-PLAN.md, and the part of the directive that
 * works exactly as asked - unlike binary plugins, a skin is pure data.
 *
 * A .wsz is a ZIP holding BMPs plus a few text files. pc64 already had both
 * halves before this: um_inflate is a callback-driven RAW inflate, which is
 * precisely what ZIP stores in method 8, and um_bmp decodes the sprite sheets.
 * So this file is a ZIP directory walk and a sprite atlas, not a decoder.
 *
 * WE SHIP OUR OWN DEFAULT ARTWORK. Stock Winamp skins load as user data; the
 * built-in look is drawn from the active unoui theme so a machine with no skin
 * installed still looks like it belongs to the desktop. That is the line
 * Audacious and XMMS took and it is the right one.
 *
 * Skins are read into a fixed arena rather than the heap: a skin is bounded
 * (the classic set is ~20 BMPs, none large) and the player must not be able to
 * exhaust memory because someone dropped in a 40 MB "skin".
 */
#include "unoamp_skin.h"
#include "unomedia.h"
#include "pc64_fs.h"
#include <string.h>

/* ---- ZIP ------------------------------------------------------------------
 * Only what a skin needs: the LOCAL FILE HEADER walk. The central directory is
 * the "right" way to enumerate a ZIP, but every .wsz in existence is a flat
 * archive written front to back, and walking local headers means one forward
 * pass over a file we are streaming anyway.
 *
 * Method 0 (stored) and method 8 (deflate) are the only ones Winamp skins use.
 */
#define ZIP_LFH 0x04034b50u

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p)
{ return p[0] | (p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24); }

/* um_inflate's shape: `in` is a PULL (fill dst, return bytes) and `out` is a
 * PUSH (here is a run). Both are trivial over a memory member - the whole
 * compressed member is already in the read buffer - so these two little
 * adapters are all the ZIP layer owes it. zlib=0, because ZIP method 8 stores
 * RAW deflate with no zlib header. */
typedef struct { const unsigned char *p; long left; } zin;
static long z_in(void *ctx, unsigned char *dst, long max)
{
    zin *s = (zin *)ctx;
    long n = s->left < max ? s->left : max;
    if (n <= 0) return 0;
    memcpy(dst, s->p, (unsigned long)n);
    s->p += n; s->left -= n;
    return n;
}
typedef struct { unsigned char *p; long cap, len; } zout;
static int z_out(void *ctx, const unsigned char *p, long n)
{
    zout *o = (zout *)ctx;
    if (o->len + n > o->cap) return 0;        /* refuse rather than overrun  */
    memcpy(o->p + o->len, p, (unsigned long)n);
    o->len += n;
    return 1;
}

static void *arena_alloc(unsigned long n);   /* the skin arena, below */

/* ---- BMP ------------------------------------------------------------------
 * A skin sheet is always an uncompressed Windows BMP, so this reads one
 * directly rather than going through unomedia's image layer. That is a
 * deliberate call, for two reasons:
 *
 *   1. um_image's decoder roster is all-or-nothing - linking it pulls PNG,
 *      JPEG, GIF, WebP and VP8 into the kernel. Paying a video-codec's worth
 *      of image for a 275x116 titlebar is not a trade worth making.
 *   2. um_image is a SINGLETON, and with BROWSER_ENGINE=uw the browser holds
 *      it while decoding <img>. A skin load must not be able to yank a decode
 *      out from under the browser.
 *
 * um_inflate IS shared - that one is standalone and ZIP method 8 is exactly
 * what it does.
 *
 * Handles 1/4/8-bit palette, 24-bit BGR and 32-bit BGRA, top-down or
 * bottom-up.
 *
 * BMP stores pixels B,G,R and the framebuffer word is 0xAABBGGRR (see FB_RGB
 * in fb.h - blue at bits 16..23, red at 0..7), so BMP byte order maps STRAIGHT
 * ACROSS with no swap. Writing the intuitive 0xAARRGGBB here would render
 * every skin with red and blue exchanged. */
static int bmp_decode(const unsigned char *p, long n, unsigned **out,
                      int *ow, int *oh)
{
    unsigned off, hdr, bpp, comp, pal_n, rowb;
    int w, h, top_down, x, y;
    const unsigned char *pal, *bits;
    unsigned *px;

    if (n < 54 || p[0] != 'B' || p[1] != 'M') return 0;
    off  = rd32(p + 10);
    hdr  = rd32(p + 14);
    if (hdr < 40 || 14 + hdr > (unsigned)n) return 0;
    w    = (int)rd32(p + 18);
    h    = (int)rd32(p + 22);
    bpp  = rd16(p + 28);
    comp = rd32(p + 30);
    if (comp != 0) return 0;               /* RLE skins do not exist          */
    top_down = h < 0;
    if (top_down) h = -h;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return 0;

    pal   = p + 14 + hdr;
    pal_n = rd32(p + 46);                  /* biClrUsed                       */
    if (!pal_n && bpp <= 8) pal_n = 1u << bpp;
    if (off > (unsigned)n) return 0;
    bits  = p + off;
    rowb  = (((unsigned)w * bpp + 31u) / 32u) * 4u;   /* rows pad to 4 bytes  */
    if ((long)(off + rowb * (unsigned)h) > n) return 0;

    px = (unsigned *)arena_alloc((unsigned long)w * (unsigned long)h * 4u);
    if (!px) return 0;

    for (y = 0; y < h; y++) {
        /* BMP stores bottom-up unless the height was negative. */
        const unsigned char *r = bits + rowb * (unsigned)(top_down ? y : h - 1 - y);
        unsigned *d = px + (long)y * w;
        for (x = 0; x < w; x++) {
            unsigned idx, b, g, bl;
            if (bpp == 32)      { bl = r[x*4]; g = r[x*4+1]; b = r[x*4+2];
                                  d[x] = 0xFF000000u | (bl<<16) | (g<<8) | b; }
            else if (bpp == 24) { bl = r[x*3]; g = r[x*3+1]; b = r[x*3+2];
                                  d[x] = 0xFF000000u | (bl<<16) | (g<<8) | b; }
            else {
                if (bpp == 8)      idx = r[x];
                else if (bpp == 4) idx = (x & 1) ? (r[x>>1] & 15u) : (r[x>>1] >> 4);
                else               idx = (r[x>>3] >> (7 - (x & 7))) & 1u;
                if (idx >= pal_n) idx = 0;
                if (pal + idx * 4u + 3u > p + n) { d[x] = 0xFF000000u; continue; }
                d[x] = 0xFF000000u | ((unsigned)pal[idx*4] << 16) |
                       ((unsigned)pal[idx*4+1] << 8) | (unsigned)pal[idx*4+2];
            }
        }
    }
    *out = px; *ow = w; *oh = h;
    return 1;
}

/* ---- the skin ------------------------------------------------------------- */
/* SIZED FROM THE FORMAT, NOT GUESSED. The classic sheet set decodes to about
 * 868 KB at 4 bytes a pixel - PLEDIT alone is 280x186, and MAIN, TITLEBAR and
 * EQMAIN are 275x116 each. A 512 KB arena (the first guess here) could not
 * hold ANY complete skin, which is the sort of bug that only shows up the
 * moment a real .wsz arrives. 2 MB leaves room for the oversized sheets some
 * skins ship without being open-ended.
 *
 * Both buffers are BSS, so they cost address space and zero file size. */
#define SKIN_ARENA (2048u * 1024u)     /* whole decoded skin, all sheets      */
static unsigned char g_arena[SKIN_ARENA];
static unsigned long g_used;
static unoamp_skin   g_skin;
static int           g_loaded;

static void *arena_alloc(unsigned long n)
{
    unsigned long at = (g_used + 15u) & ~15ul;
    if (at + n > SKIN_ARENA) return 0;
    g_used = at + n;
    return &g_arena[at];
}

/* The sheets a Winamp 2 skin is made of. Order matters only in that it is the
 * order we try to load; a skin missing any of them is still usable, which is
 * why a failed sheet is not a failed skin - Winamp itself falls back per
 * sheet, and a skin with no VOLUME.BMP should still give you a titlebar. */
static const char *kSheet[UNOAMP_SHEET_N] = {
    "MAIN.BMP", "CBUTTONS.BMP", "TITLEBAR.BMP", "SHUFREP.BMP",
    "POSBAR.BMP", "VOLUME.BMP", "BALANCE.BMP", "MONOSTER.BMP",
    "PLAYPAUS.BMP", "NUMBERS.BMP", "TEXT.BMP", "EQMAIN.BMP", "PLEDIT.BMP"
};

static int name_eq_ci(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
    return b[n] == 0;
}

/* NUMBERS.BMP is called NUMS_EX.BMP in a lot of skins (the 11-digit variant
 * with a blank). Accept either rather than losing the time display. */
static int sheet_index(const char *name, int nlen)
{
    int i;
    for (i = 0; i < UNOAMP_SHEET_N; i++)
        if (name_eq_ci(name, kSheet[i], nlen)) return i;
    if (name_eq_ci(name, "NUMS_EX.BMP", nlen)) return UNOAMP_SHEET_NUMBERS;
    return -1;
}

/* viscolor.txt: 24 lines of "r,g,b" giving the visualiser palette. Winamp uses
 * 0 as the background, 1 as the peak dot, 2..17 as the spectrum gradient and
 * 18..23 for the oscilloscope. Parsed here so phase 5 renders in the skin's
 * colours rather than inventing its own. */
static void parse_viscolor(const unsigned char *p, long n)
{
    int line = 0;
    long i = 0;
    while (i < n && line < UNOAMP_VISCOLORS) {
        int v[3], k = 0;
        for (k = 0; k < 3; k++) {
            int acc = -1;
            while (i < n && (p[i] < '0' || p[i] > '9')) {
                if (p[i] == '\n') { if (acc >= 0) break; }
                i++;
            }
            while (i < n && p[i] >= '0' && p[i] <= '9') {
                if (acc < 0) acc = 0;
                acc = acc * 10 + (p[i] - '0');
                i++;
            }
            v[k] = acc < 0 ? 0 : (acc > 255 ? 255 : acc);
        }
        /* the file says r,g,b; the framebuffer word wants b,g,r */
        g_skin.viscolor[line++] = 0xFF000000u | ((unsigned)v[2] << 16) |
                                  ((unsigned)v[1] << 8) | (unsigned)v[0];
        while (i < n && p[i] != '\n') i++;
        if (i < n) i++;
    }
    g_skin.have_viscolor = line > 0;
}

/* pledit.txt: the playlist editor's colours, as "Normal=#RRGGBB" lines. */
static unsigned hexcol(const unsigned char *p, long n, long i)
{
    unsigned v = 0; int d = 0;
    while (i < n && p[i] != '#') { if (p[i] == '\n') return 0; i++; }
    if (i >= n) return 0;
    i++;
    while (i < n && d < 6) {
        int c = p[i], x;
        if      (c >= '0' && c <= '9') x = c - '0';
        else if (c >= 'a' && c <= 'f') x = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') x = c - 'A' + 10;
        else break;
        v = (v << 4) | (unsigned)x; d++; i++;
    }
    /* #RRGGBB -> 0xAABBGGRR */
    if (d != 6) return 0;
    return 0xFF000000u | ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
}
static void parse_pledit(const unsigned char *p, long n)
{
    long i = 0;
    while (i < n) {
        if (i + 7 <= n && name_eq_ci((const char *)p + i, "NORMAL=", 7))
            g_skin.pl_normal = hexcol(p, n, i);
        else if (i + 8 <= n && name_eq_ci((const char *)p + i, "CURRENT=", 8))
            g_skin.pl_current = hexcol(p, n, i);
        else if (i + 11 <= n && name_eq_ci((const char *)p + i, "NORMALBG=", 9))
            g_skin.pl_bg = hexcol(p, n, i);
        while (i < n && p[i] != '\n') i++;
        if (i < n) i++;
    }
}

/* ---- loading -------------------------------------------------------------- */

/* One member: decode into the arena if we recognise it, skip it if not. */
static void take_member(const char *name, int nlen,
                        const unsigned char *data, long clen, long ulen,
                        int method)
{
    unsigned char *raw = 0;
    long got = 0;
    int idx;

    /* text members first - they are tiny and not BMPs */
    int is_vis = name_eq_ci(name, "VISCOLOR.TXT", nlen);
    int is_ple = name_eq_ci(name, "PLEDIT.TXT", nlen);
    idx = sheet_index(name, nlen);
    if (idx < 0 && !is_vis && !is_ple) return;

    if (method == 0) { raw = (unsigned char *)data; got = clen; }
    else if (method == 8) {
        zin  zi; zout zo;
        zi.p = data; zi.left = clen;
        raw = (unsigned char *)arena_alloc((unsigned long)ulen);
        if (!raw) return;
        zo.p = raw; zo.cap = ulen; zo.len = 0;
        if (!um_inflate(z_in, &zi, z_out, &zo, 0)) return;
        got = zo.len;
    } else return;                     /* nothing else appears in a .wsz      */

    if (is_vis) { parse_viscolor(raw, got); return; }
    if (is_ple) { parse_pledit(raw, got); return; }

    /* A sheet. A failed decode loses one sheet, never the skin: Winamp itself
     * falls back per sheet, and a skin with no VOLUME.BMP should still give
     * you a titlebar. */
    {
        unsigned *px = 0; int w = 0, h = 0;
        if (!bmp_decode(raw, got, &px, &w, &h)) return;
        g_skin.sheet[idx].px = px;
        g_skin.sheet[idx].w  = w;
        g_skin.sheet[idx].h  = h;
    }
}

int unoamp_skin_load(int vol, const char *path)
{
    /* The whole archive, resident. Real skins run 100-500 KB and a STORED
     * (uncompressed) one is larger still - FROST.wsz from tools/mkskin.py is
     * 640 KB - so this is sized for the format rather than for the smallest
     * skin that happened to be to hand. */
    static unsigned char buf[1536 * 1024];
    long n, at = 0;

    g_used = 0; g_loaded = 0;
    memset(&g_skin, 0, sizeof g_skin);

    n = uno_fs_read(vol, path, buf, (long)sizeof buf);
    if (n < 30) return 0;

    /* Walk local file headers front to back. */
    while (at + 30 <= n) {
        unsigned sig = rd32(buf + at);
        unsigned method, clen, ulen, nlen, elen;
        if (sig != ZIP_LFH) break;             /* central directory reached   */
        method = rd16(buf + at + 8);
        clen   = rd32(buf + at + 18);
        ulen   = rd32(buf + at + 22);
        nlen   = rd16(buf + at + 26);
        elen   = rd16(buf + at + 28);
        if (at + 30 + (long)nlen + (long)elen + (long)clen > n) break;
        /* A streamed ZIP can carry clen == 0 with the real sizes in a trailing
         * data descriptor. Skins are not written that way, and guessing the
         * length would mean scanning for the next signature - so refuse the
         * member rather than decode garbage. */
        if (clen > 0)
            take_member((const char *)buf + at + 30, (int)nlen,
                        buf + at + 30 + nlen + elen,
                        (long)clen, (long)ulen, (int)method);
        at += 30 + (long)nlen + (long)elen + (long)clen;
    }

    g_loaded = g_skin.sheet[UNOAMP_SHEET_MAIN].px != 0;
    return g_loaded;
}

const unoamp_skin *unoamp_skin_get(void) { return g_loaded ? &g_skin : 0; }
int unoamp_skin_loaded(void) { return g_loaded; }
void unoamp_skin_unload(void)
{ g_used = 0; g_loaded = 0; memset(&g_skin, 0, sizeof g_skin); }

/* Blit one sprite from a sheet. Clipped to both the sheet and the target;
 * a skin with a short sheet (they exist) then draws what it has instead of
 * reading past the end of the arena. */
void unoamp_skin_blit(int sheet, int sx, int sy, int w, int h,
                      unsigned *dst, int dst_w, int dst_h, int dx, int dy)
{
    const unoamp_sheet *s;
    int y;
    if (!g_loaded || sheet < 0 || sheet >= UNOAMP_SHEET_N) return;
    s = &g_skin.sheet[sheet];
    if (!s->px) return;
    for (y = 0; y < h; y++) {
        int syy = sy + y, dyy = dy + y, x;
        if (syy < 0 || syy >= s->h || dyy < 0 || dyy >= dst_h) continue;
        for (x = 0; x < w; x++) {
            int sxx = sx + x, dxx = dx + x;
            if (sxx < 0 || sxx >= s->w || dxx < 0 || dxx >= dst_w) continue;
            dst[dyy * dst_w + dxx] = s->px[syy * s->w + sxx];
        }
    }
}

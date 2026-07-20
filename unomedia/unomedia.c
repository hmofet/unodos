/* ===========================================================================
 * unomedia core - the probe/dispatch/source layer (see unomedia.h).
 *
 * One image is open at a time (the pc64_media.c precedent): a single global
 * source + decoder, no ownership questions, and decode state lives where
 * each decoder put it. The probe order runs the real decoders before the
 * identify-and-refuse stub so a valid file is never claimed by the stub.
 * ======================================================================== */
#include "unomedia.h"
#include <string.h>

/* ---- allocator ------------------------------------------------------------ */
static void *(*g_alloc)(unsigned long);
static void  (*g_free)(void *);

void um_set_alloc(void *(*a)(unsigned long), void (*f)(void *))
{ g_alloc = a; g_free = f; }

void *um_alloc(unsigned long n) { return g_alloc ? g_alloc(n) : 0; }
void  um_free(void *p)          { if (p && g_free) g_free(p); }

/* ---- error surface -------------------------------------------------------- */
static const char *g_err = "";
const char *um_error(void)             { return g_err; }
void        um_set_error(const char *w){ g_err = w ? w : ""; }

/* ---- the open source ------------------------------------------------------ */
static um_src g_src;
static int    g_src_open;

long um_size(void) { return g_src_open ? g_src.size : 0; }

long um_read(long off, unsigned char *dst, long n)
{
    if (!g_src_open || !g_src.read) return -1;
    if (off < 0 || n <= 0) return 0;
    if (off >= g_src.size) return 0;
    if (n > g_src.size - off) n = g_src.size - off;
    return g_src.read(g_src.ctx, off, dst, n);
}

/* ---- extension helpers ---------------------------------------------------- */
static void ext_of(const char *name, char *out /* [8] */)
{
    const char *dot = 0, *p;
    int i = 0;
    out[0] = 0;
    if (!name) return;
    for (p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot) return;
    for (p = dot + 1; *p && i < 7; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i++] = c;
    }
    out[i] = 0;
}

int um_image_is(const char *name)
{
    static const char *k[] = { "PNG", "JPG", "JPEG", "JFIF", "GIF", "BMP",
                               "DIB", "TGA", "PPM", "PGM", "PBM", "PNM",
                               "QOI", "ICO", "CUR" };
    char e[8];
    unsigned i;
    ext_of(name, e);
    if (!e[0]) return 0;
    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(e, k[i])) return 1;
    return 0;
}

/* ---- the decoder roster ---------------------------------------------------
 * Probe order matters only for ambiguity: ICO before BMP (an ICO's payload
 * is headerless BMP), the stub last (it exists to catch what nothing else
 * claimed but a human would still call an image). */
static const um_idecoder *const kDec[] = {
    &um_idec_png, &um_idec_jpg, &um_idec_gif, &um_idec_ico, &um_idec_bmp,
    &um_idec_qoi, &um_idec_pnm, &um_idec_tga, &um_idec_stub,
};
#define NDEC ((int)(sizeof kDec / sizeof kDec[0]))

static const um_idecoder *g_dec;

int um_image_open(const um_src *src, const char *name, um_image_info *info)
{
    unsigned char head[UM_IMG_HEAD];
    char ext[8];
    long n;
    int i;

    um_image_close();
    um_set_error("");
    if (!src || !src->read || !info) { um_set_error("bad call"); return 0; }
    g_src = *src;
    g_src_open = 1;

    n = um_read(0, head, UM_IMG_HEAD);
    if (n <= 0) { um_set_error("empty file"); g_src_open = 0; return 0; }
    ext_of(name, ext);

    memset(info, 0, sizeof *info);
    for (i = 0; i < NDEC; i++) {
        if (!kDec[i]->probe(head, n, ext)) continue;
        g_dec = kDec[i];
        if (!g_dec->open(info)) {
            if (!um_error()[0]) um_set_error("malformed image");
            g_dec = 0; g_src_open = 0;
            return 0;
        }
        /* central sanity net - decoders check too, but nothing downstream
           may ever see dimensions that overflow w*h*4 */
        if (info->w <= 0 || info->h <= 0 ||
            info->w > UM_MAX_DIM || info->h > UM_MAX_DIM ||
            (long)info->w * info->h > UM_MAX_PIXELS) {
            um_set_error("image dimensions out of range");
            g_dec->close(); g_dec = 0; g_src_open = 0;
            return 0;
        }
        return 1;
    }
    um_set_error("not a recognised image format");
    g_src_open = 0;
    return 0;
}

int um_image_frame(um_px *dst, int *delay_ms)
{
    int d = 0, r;
    if (!g_dec || !dst) return -1;
    r = g_dec->frame(dst, &d);
    if (delay_ms) *delay_ms = d;
    return r;
}

void um_image_rewind(void)
{
    if (g_dec && g_dec->rewind) g_dec->rewind();
}

void um_image_close(void)
{
    if (g_dec) g_dec->close();
    g_dec = 0;
    g_src_open = 0;
}

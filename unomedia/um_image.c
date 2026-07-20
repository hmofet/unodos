/* ===========================================================================
 * unomedia - the IMAGE dispatcher (probe + vtable routing; see unomedia.h).
 *
 * One image is open at a time per instance (the pc64_media.c precedent): a
 * single decoder slot, no ownership questions, decode state lives in each
 * decoder's file. The probe order runs the real decoders before the
 * identify-and-refuse stub so a valid file is never claimed by the stub.
 * ======================================================================== */
#include "unomedia.h"
#include "unomedia_int.h"
#include <string.h>

int um_image_is(const char *name)
{
    static const char *k[] = { "PNG", "JPG", "JPEG", "JFIF", "GIF", "BMP",
                               "DIB", "TGA", "PPM", "PGM", "PBM", "PNM",
                               "QOI", "ICO", "CUR",
                               "WEBP", "WEB" /* .webp truncated to 8.3 */ };
    char e[8];
    unsigned i;
    um_ext_of(name, e);
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
    &um_idec_png, &um_idec_jpg, &um_idec_gif, &um_idec_webp, &um_idec_ico,
    &um_idec_bmp, &um_idec_qoi, &um_idec_pnm, &um_idec_tga, &um_idec_stub,
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
    if (!info) { um_set_error("bad call"); return 0; }
    if (!um_src_open(src, UM_OWNER_IMAGE)) return 0;

    n = um_read(0, head, UM_IMG_HEAD);
    if (n <= 0) { um_set_error("empty file"); um_src_close(UM_OWNER_IMAGE); return 0; }
    um_ext_of(name, ext);

    memset(info, 0, sizeof *info);
    for (i = 0; i < NDEC; i++) {
        if (!kDec[i]->probe(head, n, ext)) continue;
        g_dec = kDec[i];
        if (!g_dec->open(info)) {
            if (!um_error()[0]) um_set_error("malformed image");
            g_dec = 0; um_src_close(UM_OWNER_IMAGE);
            return 0;
        }
        /* central sanity net - decoders check too, but nothing downstream
           may ever see dimensions that overflow w*h*4 */
        if (info->w <= 0 || info->h <= 0 ||
            info->w > UM_MAX_DIM || info->h > UM_MAX_DIM ||
            (long)info->w * info->h > UM_MAX_PIXELS) {
            um_set_error("image dimensions out of range");
            g_dec->close(); g_dec = 0; um_src_close(UM_OWNER_IMAGE);
            return 0;
        }
        return 1;
    }
    um_set_error("not a recognised image format");
    um_src_close(UM_OWNER_IMAGE);
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
    um_src_close(UM_OWNER_IMAGE);
}

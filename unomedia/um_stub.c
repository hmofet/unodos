/* ===========================================================================
 * unomedia - the identify-and-refuse decoder (the AAC precedent, for
 * images).
 *
 * Formats a human would call an image but this build does not decode are
 * still RECOGNISED, so the viewer can say "WebP recognised - no decoder in
 * this build" instead of the false "not an image". Probed last: it never
 * shadows a real decoder, it only names what nothing else claimed.
 *
 * Why these stay undecoded (for now):
 *   AVIF/HEIC - AV1/HEVC intra codecs; each a from-scratch video-codec
 *   effort (and HEVC's patent pools are alive - a licensing hazard, not
 *   just work). WebP graduated out of this list: um_webp.c decodes it.
 *   TIFF      - a container of many codecs (LZW, packbits, JPEG, ZIP);
 *   JPEG XL   - ditto, plus a very young spec.
 *   SVG       - not a raster format; a document. Earmarked for unodoc,
 *               the planned document-format library (see README).
 * ======================================================================== */
#include "unomedia.h"
#include <string.h>

static const char *g_why;

static int stub_probe(const unsigned char *h, long n, const char *ext)
{
    (void)ext;
    g_why = 0;
    if (n >= 4 && (!memcmp(h, "II*\0", 4) || !memcmp(h, "MM\0*", 4)))
        g_why = "TIFF recognised - no decoder in this build";
    else if (n >= 12 && !memcmp(h + 4, "ftyp", 4)) {
        if (!memcmp(h + 8, "avif", 4) || !memcmp(h + 8, "avis", 4))
            g_why = "AVIF recognised - no decoder in this build";
        else if (!memcmp(h + 8, "heic", 4) || !memcmp(h + 8, "heix", 4) ||
                 !memcmp(h + 8, "mif1", 4))
            g_why = "HEIC recognised - no decoder in this build";
    }
    else if ((n >= 2 && h[0] == 0xFF && h[1] == 0x0A) ||
             (n >= 12 && !memcmp(h, "\0\0\0\x0CJXL \x0D\x0A\x87\x0A", 12)))
        g_why = "JPEG XL recognised - no decoder in this build";
    else if (n >= 5 && (!memcmp(h, "<svg", 4) || !memcmp(h, "<?xml", 5)))
        g_why = "SVG is a vector format - no renderer in this build";
    return g_why != 0;
}

static int stub_open(um_image_info *info)
{
    (void)info;
    um_set_error(g_why ? g_why : "unsupported image format");
    return 0;
}

static int  stub_frame(um_px *d, int *ms) { (void)d; (void)ms; return -1; }
static void stub_close(void) {}

const um_idecoder um_idec_stub =
    { "stub", stub_probe, stub_open, stub_frame, 0, stub_close };

/* ===========================================================================
 * UNOAUTOMATE screen grab - QOI encoder over the software framebuffer.
 *
 * See unoauto_screen.h. This is a self-contained QOI *encoder* (unomedia ships
 * decoders only, and is a different lane - AGENTS.md §1/§2, so we do not reach
 * into it). QOI: "qoif" magic, big-endian dims, then a byte stream of ops with
 * a 64-entry running-colour hash + a run length. Spec mirrored from the decoder
 * in unomedia/um_qoi.c. The whole file is UNO_DEBUG-only, like URC.
 * ======================================================================== */
#include "unoauto_screen.h"

#ifdef UNO_DEBUG

#include "fb.h"          /* fb[], uno_fb_w/uno_fb_h, FB_W/FB_H */
#include <stdint.h>

void uno_screen_size(int *w, int *h)
{
    if (w) *w = FB_W;
    if (h) *h = FB_H;
}

/* QOI opcodes (see the spec / um_qoi.c). */
#define QOI_OP_INDEX 0x00   /* 00xxxxxx */
#define QOI_OP_DIFF  0x40   /* 01xxxxxx */
#define QOI_OP_LUMA  0x80   /* 10xxxxxx */
#define QOI_OP_RUN   0xc0   /* 11xxxxxx */
#define QOI_OP_RGB   0xfe
#define QOI_OP_RGBA  0xff

typedef struct { unsigned char r, g, b, a; } qpx;

static int qhash(qpx p)      /* running-array index */
{
    return (p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11) & 63;
}

int uno_screen_grab_qoi(int scale, unsigned char *out, int cap, int *ow, int *oh)
{
    int W = FB_W, H = FB_H, x, y, o = 0, run = 0;
    int ew, eh;
    qpx index[64], prev;

    if (scale < 1) scale = 1;
    ew = W / scale;
    eh = H / scale;
    if (ew < 1) ew = 1;
    if (eh < 1) eh = 1;
    if (ow) *ow = ew;
    if (oh) *oh = eh;

    /* header: magic, width, height (both big-endian), channels=4, colorspace=0 */
    if (cap < 14 + 8) return -1;
    out[o++] = 'q'; out[o++] = 'o'; out[o++] = 'i'; out[o++] = 'f';
    out[o++] = (unsigned char)(ew >> 24); out[o++] = (unsigned char)(ew >> 16);
    out[o++] = (unsigned char)(ew >> 8);  out[o++] = (unsigned char)ew;
    out[o++] = (unsigned char)(eh >> 24); out[o++] = (unsigned char)(eh >> 16);
    out[o++] = (unsigned char)(eh >> 8);  out[o++] = (unsigned char)eh;
    out[o++] = 4;    /* channels  */
    out[o++] = 0;    /* colorspace */

    for (y = 0; y < 64; y++) { index[y].r = index[y].g = index[y].b = index[y].a = 0; }
    prev.r = prev.g = prev.b = 0; prev.a = 255;

    /* leave 8 bytes of headroom for the end marker + the largest single op (5) */
    for (y = 0; y < eh; y++) {
        const fb_px *row = &fb[(y * scale) * FB_W];
        for (x = 0; x < ew; x++) {
            fb_px v = row[x * scale];
            qpx px;
            px.r = (unsigned char)(v & 0xff);
            px.g = (unsigned char)((v >> 8) & 0xff);
            px.b = (unsigned char)((v >> 16) & 0xff);
            px.a = (unsigned char)((v >> 24) & 0xff);

            if (o + 8 > cap) return -1;

            if (px.r == prev.r && px.g == prev.g && px.b == prev.b && px.a == prev.a) {
                run++;
                if (run == 62) { out[o++] = (unsigned char)(QOI_OP_RUN | (run - 1)); run = 0; }
            } else {
                int ip;
                if (run > 0) { out[o++] = (unsigned char)(QOI_OP_RUN | (run - 1)); run = 0; }
                ip = qhash(px);
                if (index[ip].r == px.r && index[ip].g == px.g &&
                    index[ip].b == px.b && index[ip].a == px.a) {
                    out[o++] = (unsigned char)(QOI_OP_INDEX | ip);
                } else {
                    index[ip] = px;
                    if (px.a == prev.a) {
                        int vr = (int)px.r - (int)prev.r;
                        int vg = (int)px.g - (int)prev.g;
                        int vb = (int)px.b - (int)prev.b;
                        int dr_dg = vr - vg, db_dg = vb - vg;
                        if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
                            out[o++] = (unsigned char)(QOI_OP_DIFF |
                                ((vr + 2) << 4) | ((vg + 2) << 2) | (vb + 2));
                        } else if (dr_dg > -9 && dr_dg < 8 && vg > -33 && vg < 32 &&
                                   db_dg > -9 && db_dg < 8) {
                            out[o++] = (unsigned char)(QOI_OP_LUMA | (vg + 32));
                            out[o++] = (unsigned char)(((dr_dg + 8) << 4) | (db_dg + 8));
                        } else {
                            out[o++] = QOI_OP_RGB;
                            out[o++] = px.r; out[o++] = px.g; out[o++] = px.b;
                        }
                    } else {
                        out[o++] = QOI_OP_RGBA;
                        out[o++] = px.r; out[o++] = px.g; out[o++] = px.b; out[o++] = px.a;
                    }
                }
                prev = px;
            }
        }
    }
    if (run > 0) {
        if (o + 8 > cap) return -1;
        out[o++] = (unsigned char)(QOI_OP_RUN | (run - 1));
    }
    /* 8-byte end marker: seven 0x00 then 0x01 */
    if (o + 8 > cap) return -1;
    out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 0;
    out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 1;
    return o;
}

#endif /* UNO_DEBUG */

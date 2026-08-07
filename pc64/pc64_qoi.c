/* ===========================================================================
 * UnoDOS/pc64 - a QOI decoder, small enough to live in the kernel.
 *
 * WHY QOI AND NOT PNG.  An app that ships from disk should be able to ship its
 * own icon, and the shell has to be able to draw that icon before it will load
 * a single byte of the app's code - so the decoder cannot live in a module.
 * PNG means inflate, which is far too much to put in the kernel for artwork.
 * The OS ALREADY SPEAKS QOI: unoauto_screen.c encodes the framebuffer in it for
 * remote desktop.  This is the other half of a format the project already
 * carries, in about eighty lines, with no tables and no allocation.
 *
 * The full unomedia decoder set (PNG, JPEG, GIF, ...) stays where it belongs,
 * inside PHOTOS.UNO; this is deliberately only what a 32x32 emblem needs.
 *
 * Format: "qoif", u32be width, u32be height, u8 channels, u8 colorspace, then
 * a byte stream of ops, then eight bytes 00...01.
 * ======================================================================== */
#include "pc64_qoi.h"

#define OP_INDEX 0x00
#define OP_DIFF  0x40
#define OP_LUMA  0x80
#define OP_RUN   0xC0
#define OP_RGB   0xFE
#define OP_RGBA  0xFF

static unsigned rd32(const unsigned char *p)
{ return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
         ((unsigned)p[2] << 8) | p[3]; }

int uno_qoi_decode(const unsigned char *in, long n, unsigned char *out,
                   int maxw, int maxh, int *w_out, int *h_out)
{
    unsigned char idx[64][4];
    unsigned char px[4] = { 0, 0, 0, 255 };
    unsigned w, h;
    long p = 14;
    int i, run = 0, npx, k;

    if (!in || !out || n < 22) return -1;
    if (in[0] != 'q' || in[1] != 'o' || in[2] != 'i' || in[3] != 'f') return -1;
    w = rd32(in + 4); h = rd32(in + 8);
    if (!w || !h || w > (unsigned)maxw || h > (unsigned)maxh) return -1;
    if (in[12] != 3 && in[12] != 4) return -1;
    npx = (int)(w * h);

    for (i = 0; i < 64; i++) { idx[i][0] = idx[i][1] = idx[i][2] = idx[i][3] = 0; }

    for (i = 0; i < npx; i++) {
        if (run > 0) {
            run--;
        } else {
            int b;
            if (p >= n) return -1;
            b = in[p++];
            if (b == OP_RGB) {
                if (p + 3 > n) return -1;
                px[0] = in[p]; px[1] = in[p+1]; px[2] = in[p+2]; p += 3;
            } else if (b == OP_RGBA) {
                if (p + 4 > n) return -1;
                px[0] = in[p]; px[1] = in[p+1]; px[2] = in[p+2]; px[3] = in[p+3]; p += 4;
            } else if ((b & 0xC0) == OP_INDEX) {
                for (k = 0; k < 4; k++) px[k] = idx[b & 0x3F][k];
            } else if ((b & 0xC0) == OP_DIFF) {
                px[0] = (unsigned char)(px[0] + ((b >> 4) & 3) - 2);
                px[1] = (unsigned char)(px[1] + ((b >> 2) & 3) - 2);
                px[2] = (unsigned char)(px[2] + (b & 3) - 2);
            } else if ((b & 0xC0) == OP_LUMA) {
                int b2, vg;
                if (p >= n) return -1;
                b2 = in[p++];
                vg = (b & 0x3F) - 32;
                px[0] = (unsigned char)(px[0] + vg - 8 + ((b2 >> 4) & 0x0F));
                px[1] = (unsigned char)(px[1] + vg);
                px[2] = (unsigned char)(px[2] + vg - 8 + (b2 & 0x0F));
            } else {                                        /* OP_RUN */
                run = b & 0x3F;                             /* bias -1 */
            }
            { int hsh = (px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) & 63;
              for (k = 0; k < 4; k++) idx[hsh][k] = px[k]; }
        }
        for (k = 0; k < 4; k++) out[i * 4 + k] = px[k];
    }
    if (w_out) *w_out = (int)w;
    if (h_out) *h_out = (int)h;
    return 0;
}

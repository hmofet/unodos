/* QOI decode, for app icons that ship alongside a .UNO (pc64_qoi.c).
 *
 * The shell has to draw an app's icon BEFORE it would ever load that app's
 * code, so this cannot live in a module - and PNG's inflate is too much to put
 * in the kernel for artwork. The OS already encodes QOI for remote desktop
 * (unoauto_screen.c); this is the other half of a format it already speaks.
 * Everything richer stays in unomedia, inside PHOTOS.UNO. */
#ifndef PC64_QOI_H
#define PC64_QOI_H

/* Decode `in[0..n)` into `out` as RGBA rows, at most maxw x maxh.
 * 0 on success (w_out and h_out get the real size), -1 on anything malformed,
 * too large or not QOI. Allocates nothing and never writes past
 * maxw*maxh*4 bytes. */
int uno_qoi_decode(const unsigned char *in, long n, unsigned char *out,
                   int maxw, int maxh, int *w_out, int *h_out);

#endif

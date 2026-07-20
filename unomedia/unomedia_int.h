/* ===========================================================================
 * unomedia - internal cross-decoder hooks. NOT part of the public surface.
 *
 * ICO is a container: its payload is either a headerless BMP (a bare DIB
 * with the height doubled for the AND mask) or, since Vista, a whole PNG
 * stream. Rather than let the ICO decoder grow private copies of both, the
 * BMP and PNG decoders each export an "open at byte offset" entry and ICO
 * delegates - open picks the payload, frame/close forward to it.
 * ======================================================================== */
#ifndef UNOMEDIA_INT_H
#define UNOMEDIA_INT_H

#include "unomedia.h"

/* ---- core internals (unomedia.c) ------------------------------------------
 * The single open byte source is owner-tagged: the image and audio
 * dispatchers claim it with their own tag, so a stray open from the other
 * family fails loudly instead of silently stealing the stream. */
enum { UM_OWNER_IMAGE = 1, UM_OWNER_AUDIO = 2 };
int  um_src_open(const um_src *src, int owner);   /* 0 = refused (um_error) */
void um_src_close(int owner);
void um_ext_of(const char *name, char *out /* [8] */);

/* PNG stream starting at absolute byte offset `off` in the source (0 = a
 * plain .png file). After a successful open, um_idec_png.frame/close apply. */
int um_png_open_at(long off, um_image_info *info);

/* A bare DIB (BITMAPINFOHEADER and successors, no BITMAPFILEHEADER) at
 * absolute offset `off`, pixel data ending before `end` (<=0 = end of file).
 * `ico` != 0 applies the ICO rules: biHeight is doubled (XOR + AND masks),
 * and for depths < 32 the 1-bit AND mask supplies transparency. After a
 * successful open, um_idec_bmp.frame/close apply. */
int um_bmp_open_dib(long off, long end, int ico, um_image_info *info);

#endif /* UNOMEDIA_INT_H */

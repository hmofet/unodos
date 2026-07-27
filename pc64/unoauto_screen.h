/* ===========================================================================
 * UNOAUTOMATE screen grab - the OUT half of URC remote desktop.
 *
 * URC already injects input (key/pointer); this is the missing other half:
 * snapshot the live UnoDOS framebuffer (fb.h `fb[]`) so a dev-PC client can
 * render it. The frame is QOI-encoded (lossless, tiny on UnoDOS's flat-colour
 * desktop, trivial to decode) and the `screen` verb in unoauto_remote.c streams
 * it base64 like `readsec`. UNO_DEBUG-only, same gate as the rest of URC.
 * ======================================================================== */
#ifndef UNOAUTO_SCREEN_H
#define UNOAUTO_SCREEN_H

/* Current desktop size (FB_W x FB_H) - for the `screen info` reply. */
void uno_screen_size(int *w, int *h);

/* Encode the current framebuffer as a QOI image into `out` (up to `cap` bytes).
 * `scale` >= 1 downsamples nearest-neighbour (2 => half width & height, a
 * quarter of the pixels), so a busy/hi-res screen can still fit the caller's
 * buffer. Writes the emitted image dimensions to *ow/*oh. Returns the QOI byte
 * count, or -1 if it did not fit in `cap` (the caller raises `scale` and
 * retries). fb[] pixels are 0xAABBGGRR (R,G,B,A byte order in memory), fed
 * straight into QOI's RGBA channels. */
int  uno_screen_grab_qoi(int scale, unsigned char *out, int cap, int *ow, int *oh);

#endif

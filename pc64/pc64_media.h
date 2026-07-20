/* ===========================================================================
 * UnoDOS/pc64 - the media ADAPTER.
 *
 * The decoders and the probe/vtable layer that used to live here moved to
 * the shared library (../unomedia - see its README): unomedia owns formats,
 * this file owns the pc64 plumbing. What remains is exactly the glue the
 * kernel needs: a 64 KB sliding-window byte source over uno_fs_read_at (so
 * a 40 MB WAV costs the same resident memory as a 3 MB MP3), the kernel
 * allocator wiring, and one open call. Decode/seek/position/close are
 * driven straight through unomedia's um_audio_* surface.
 * ======================================================================== */
#ifndef PC64_MEDIA_H
#define PC64_MEDIA_H

#include "unomedia.h"

/* Open `name` on fs volume `vol` through the windowed source and probe it
 * as audio. 1 = ready (drive um_audio_decode/seek_ms/pos_ms/close from
 * unomedia.h); 0 = failed, um_error() says why. */
int pc64_media_open(int vol, const char *name, um_audio_info *info);

#endif

/* ===========================================================================
 * UnoDOS/pc64 - the media adapter (see pc64_media.h).
 *
 * The byte source is a single sliding window over the file. Decoders read
 * with absolute offsets and are free to seek backwards - the window refills
 * from the file system when a request falls outside it. Sequential decoding,
 * which is what every decoder does, hits the window almost every time.
 * ======================================================================== */
#include "pc64_media.h"
#include "pc64_fs.h"
#include <string.h>
#include <stdlib.h>

#define SRC_WIN 65536                    /* sliding window over the file       */

static int   g_vol = -1;
static char  g_path[132];
static unsigned char g_win[SRC_WIN];
static long  g_win_off = -1;             /* file offset of g_win[0]            */
static long  g_win_len;

static long win_read(void *ctx, long off, unsigned char *dst, long n)
{
    long done = 0;
    (void)ctx;
    if (off < 0 || n <= 0 || g_vol < 0) return 0;
    while (done < n) {
        long want = off + done, avail;
        if (g_win_off < 0 || want < g_win_off || want >= g_win_off + g_win_len) {
            /* refill: start the window at the request so a forward scan runs
               to the end of it before the next read hits the disk again */
            g_win_off = want;
            g_win_len = uno_fs_read_at(g_vol, g_path, g_win_off, g_win, SRC_WIN);
            if (g_win_len <= 0) { g_win_off = -1; g_win_len = 0; break; }
        }
        avail = g_win_off + g_win_len - want;
        if (avail > n - done) avail = n - done;
        memcpy(dst + done, g_win + (want - g_win_off), (unsigned long)avail);
        done += avail;
    }
    return done;
}

int pc64_media_open(int vol, const char *name, um_audio_info *info)
{
    um_src src;
    long size = uno_fs_size(vol, name);
    if (size <= 0) { um_set_error("cannot read file"); return 0; }

    um_set_alloc(malloc, free);          /* idempotent kernel-heap wiring */
    g_vol = vol;
    strncpy(g_path, name, sizeof g_path - 1);
    g_path[sizeof g_path - 1] = 0;
    g_win_off = -1; g_win_len = 0;

    src.read = win_read; src.size = size; src.ctx = 0;
    if (!um_audio_open(&src, name, info)) { g_vol = -1; return 0; }
    return 1;
}

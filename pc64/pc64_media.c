/* ===========================================================================
 * UnoDOS/pc64 - the media layer: format probe + the streaming byte source.
 *
 * The probe reads the first UNO_MEDIA_HEAD bytes once and offers them to each
 * decoder in turn; a decoder claims a file by magic bytes, falling back to the
 * extension for formats whose magic is weak (raw AAC in ADTS, for instance,
 * is just a 12-bit sync word that random data hits often enough to matter).
 *
 * The byte source is a single sliding window over the file. Decoders read
 * with absolute offsets and are free to seek backwards - the window refills
 * from the file system when a request falls outside it. Sequential decoding,
 * which is what every decoder here does, hits the window almost every time.
 * ======================================================================== */
#include "pc64_media.h"
#include "pc64_fs.h"
#include <string.h>

/* ---- the byte source ------------------------------------------------------ */
#define SRC_WIN 65536                    /* sliding window over the file       */

static int   g_vol = -1;
static char  g_path[132];
static long  g_size;
static unsigned char g_win[SRC_WIN];
static long  g_win_off = -1;             /* file offset of g_win[0]            */
static long  g_win_len;

long uno_src_size(void) { return g_size; }

long uno_src_read(long off, unsigned char *dst, long n)
{
    long done = 0;
    if (off < 0 || n <= 0 || g_vol < 0) return 0;
    if (off >= g_size) return 0;
    if (n > g_size - off) n = g_size - off;
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

/* ---- extension helpers ---------------------------------------------------- */
static void ext_of(const char *name, char *out, int max)
{
    const char *d = 0, *s = name;
    int i = 0;
    while (*s) { if (*s == '.') d = s; s++; }
    if (d) {
        for (d++; *d && i < max - 1; d++) {
            char c = *d;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out[i++] = c;
        }
    }
    out[i] = 0;
}

static const char *kAudioExt[] = {
    "WAV", "MID", "MIDI", "RMI", "MP3", "AAC", "M4A", "MP4", "ADT", 0
};

int uno_media_is_audio(const char *name)
{
    char e[8]; int i;
    ext_of(name, e, sizeof e);
    for (i = 0; kAudioExt[i]; i++) if (!strcmp(e, kAudioExt[i])) return 1;
    return 0;
}

/* ---- the decoder table ---------------------------------------------------- */
static const uno_decoder *kDecoders[] = {
    &uno_dec_wav,
    &uno_dec_midi,
    &uno_dec_mp3,
    /* AAC is present for IDENTIFICATION only: it parses the container and
     * reports what the file is, then declines. See dec_aac.c for why there is
     * no decode core. */
    &uno_dec_aac,
    0
};

static const uno_decoder *g_dec;
static const char *g_err = "";

const char *uno_media_error(void)          { return g_err; }
void        uno_media_set_error(const char *w) { g_err = w ? w : ""; }

int uno_media_open(int vol, const char *name, uno_media_info *info)
{
    unsigned char head[UNO_MEDIA_HEAD];
    char e[8];
    long n;
    int i;

    uno_media_close();
    g_err = "";

    g_size = uno_fs_size(vol, name);
    if (g_size <= 0) return 0;
    g_vol = vol;
    strncpy(g_path, name, sizeof g_path - 1);
    g_path[sizeof g_path - 1] = 0;
    g_win_off = -1; g_win_len = 0;

    n = uno_src_read(0, head, UNO_MEDIA_HEAD);
    if (n <= 0) { g_vol = -1; return 0; }
    ext_of(name, e, sizeof e);

    memset(info, 0, sizeof *info);
    info->duration_ms = -1;
    info->channels = 2;
    info->rate = 44100;

    for (i = 0; kDecoders[i]; i++) {
        if (!kDecoders[i]->probe(head, n, e)) continue;
        if (!kDecoders[i]->open(info)) continue;      /* claimed but malformed */
        g_dec = kDecoders[i];
        info->format = g_dec->name;
        return 1;
    }
    g_vol = -1;
    return 0;
}

int uno_media_decode(short *out, int max_frames)
{
    if (!g_dec) return 0;
    return g_dec->decode(out, max_frames);
}

int uno_media_seek_ms(long ms)
{
    if (!g_dec || !g_dec->seek) return 0;
    return g_dec->seek(ms);
}

long uno_media_pos_ms(void)
{
    if (!g_dec || !g_dec->pos_ms) return 0;
    return g_dec->pos_ms();
}

void uno_media_close(void)
{
    if (g_dec && g_dec->close) g_dec->close();
    g_dec = 0;
    g_vol = -1;
    g_size = 0;
    g_win_off = -1;
    g_win_len = 0;
}

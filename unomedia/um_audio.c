/* ===========================================================================
 * unomedia - the AUDIO dispatcher (probe + vtable routing; see unomedia.h).
 *
 * The shape is pc64_media.c's, verbatim by design - these decoders began
 * life there and the Music app's expectations (one open stream, s16 frames
 * decoded on demand, seek by milliseconds, an error surface that names WHY
 * a recognised file was declined) are the contract. The probe reads the
 * first UM_AUD_HEAD bytes once and offers them to each decoder; magic
 * bytes first, extension as the tiebreak for weak-magic formats (raw ADTS
 * AAC's 12-bit syncword hits random data often enough to matter).
 * ======================================================================== */
#include "unomedia.h"
#include "unomedia_int.h"
#include <string.h>

int um_audio_is(const char *name)
{
    static const char *k[] = { "WAV", "MID", "MIDI", "RMI", "MP3",
                               "AAC", "M4A", "MP4", "ADT" };
    char e[8];
    unsigned i;
    um_ext_of(name, e);
    if (!e[0]) return 0;
    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(e, k[i])) return 1;
    return 0;
}

/* ---- the decoder roster ---------------------------------------------------- */
static const um_adecoder *const kDec[] = {
    &um_adec_wav, &um_adec_midi, &um_adec_mp3, &um_adec_aac,
};
#define NDEC ((int)(sizeof kDec / sizeof kDec[0]))

static const um_adecoder *g_dec;

int um_audio_open(const um_src *src, const char *name, um_audio_info *info)
{
    unsigned char head[UM_AUD_HEAD];
    char ext[8];
    long n;
    int i;

    um_audio_close();
    um_set_error("");
    if (!info) { um_set_error("bad call"); return 0; }
    if (!um_src_open(src, UM_OWNER_AUDIO)) return 0;

    n = um_read(0, head, UM_AUD_HEAD);
    if (n <= 0) { um_set_error("empty file"); um_src_close(UM_OWNER_AUDIO); return 0; }
    um_ext_of(name, ext);

    memset(info, 0, sizeof *info);
    info->duration_ms = -1;
    info->channels = 2;
    info->rate = 44100;

    for (i = 0; i < NDEC; i++) {
        if (!kDec[i]->probe(head, n, ext)) continue;
        if (!kDec[i]->open(info)) continue;      /* claimed but malformed */
        g_dec = kDec[i];
        info->format = g_dec->name;
        return 1;
    }
    if (!um_error()[0]) um_set_error("not a recognised audio format");
    um_src_close(UM_OWNER_AUDIO);
    return 0;
}

int um_audio_decode(short *out, int max_frames)
{
    if (!g_dec) return 0;
    return g_dec->decode(out, max_frames);
}

int um_audio_seek_ms(long ms)
{
    if (!g_dec || !g_dec->seek) return 0;
    return g_dec->seek(ms);
}

long um_audio_pos_ms(void)
{
    if (!g_dec || !g_dec->pos_ms) return 0;
    return g_dec->pos_ms();
}

void um_audio_close(void)
{
    if (g_dec && g_dec->close) g_dec->close();
    g_dec = 0;
    um_src_close(UM_OWNER_AUDIO);
}

/* ===========================================================================
 * UnoDOS/pc64 - the in-memory score player (see snd_mus.h).
 *
 * Three things, and nothing else: hold the caller's bytes, point unomedia's
 * MIDI decoder at them through a memory byte source, and pump the result
 * into the PCM stream the same way pc64_music.c pumps a file.
 *
 * TWO PRODUCERS, ONE RING. The Music app and this player both want the
 * sample stream, and both drive the SAME unomedia audio instance (the kernel
 * links one; PHOTOS.UNO carries its own). So:
 *   - play() refuses while any stream is open - the user's Music app wins by
 *     virtue of being there first, and the game falls back to its beeps.
 *   - tick() re-checks the owner every frame. If Music has taken the ring
 *     since, this player drops its state WITHOUT closing the decoder: the
 *     decoder is no longer ours to close, and closing it would silence the
 *     app that just took over.
 * ======================================================================== */
#include "snd_mus.h"
#include "snd_pcm.h"
#include "unomedia.h"
#include <stdlib.h>
#include <string.h>

#define MUS_MAX   (1024u * 1024)     /* a level's score; D_E1M8 is ~66 KB    */
#define MUS_BLK   1024               /* frames per decode call               */
#define MUS_TICK  8192               /* ...and per shell frame: priming the
                                        FIFO in one go would stall a frame   */

static unsigned char *g_smf;         /* our copy of the caller's file        */
static long  g_smf_len;
static int   g_playing;
static int   g_loop;
static short g_pcm[MUS_BLK * 2];

/* unomedia's allocator hook takes `unsigned long`, which is not this
   target's size_t (LLP64: 32 vs 64 bits), so malloc cannot be handed over
   directly without a function-pointer type mismatch. */
static void *mus_alloc(unsigned long n) { return malloc((size_t)n); }
static void  mus_free(void *p)          { free(p); }

static long smf_read(void *ctx, long off, unsigned char *dst, long n)
{
    long len = g_smf_len;
    (void)ctx;                                /* the buffer IS the state      */
    if (!g_smf || off < 0 || off >= len) return 0;
    if (n > len - off) n = len - off;
    memcpy(dst, g_smf + off, (unsigned long)n);
    return n;
}

/* Let go. `close_dec` = 0 when the decoder has been taken over by somebody
   else, in which case it is theirs and we must not touch it. */
static void mus_drop(int close_dec)
{
    if (close_dec) {
        um_audio_close();
        uno_snd_stream_flush();
        uno_snd_stream_end();
    }
    free(g_smf);
    g_smf = 0;
    g_smf_len = 0;
    g_playing = 0;
}

int uno_snd_mus_play(const unsigned char *smf, long len, int loop)
{
    um_src src;
    um_audio_info info;

    uno_snd_mus_stop();
    if (!smf || len < 14 || len > (long)MUS_MAX) return 0;
    if (!uno_snd_active()) return 0;             /* no DAC, no point          */
    if (uno_snd_stream_open()) return 0;         /* somebody else has the ring*/

    g_smf = (unsigned char *)malloc((unsigned long)len);
    if (!g_smf) return 0;
    memcpy(g_smf, smf, (unsigned long)len);
    g_smf_len = len;

    um_set_alloc(mus_alloc, mus_free);           /* idempotent, as elsewhere  */
    src.read = smf_read;
    src.size = len;
    src.ctx  = 0;
    if (!um_audio_open(&src, "score.mid", &info)) {
        free(g_smf); g_smf = 0; g_smf_len = 0; return 0;
    }

    uno_snd_stream_begin_owned(UNO_SND_OWN_MUS, info.rate, info.channels);
    g_playing = 1;
    g_loop    = loop ? 1 : 0;
    uno_snd_mus_tick();                          /* prime one block now       */
    return 1;
}

void uno_snd_mus_stop(void)
{
    if (!g_playing) return;
    mus_drop(uno_snd_stream_open() &&
             uno_snd_stream_owner() == UNO_SND_OWN_MUS);
}

int uno_snd_mus_playing(void) { return g_playing; }

void uno_snd_mus_tick(void)
{
    int space, done = 0, restarts = 0;
    if (!g_playing) return;
    if (!uno_snd_stream_open() || uno_snd_stream_owner() != UNO_SND_OWN_MUS) {
        mus_drop(0);                             /* displaced: see the header */
        return;
    }
    while (done < MUS_TICK && (space = uno_snd_stream_space()) > 0) {
        int want = space > MUS_BLK ? MUS_BLK : space;
        int got  = um_audio_decode(g_pcm, want);
        if (got <= 0) {                          /* end of the score          */
            /* Loop by seeking home. The bound is not paranoia: a file that
               opens but decodes nothing would otherwise spin here forever. */
            if (g_loop && restarts < 2 && um_audio_seek_ms(0)) { restarts++; continue; }
            if (uno_snd_stream_queued() > 0) break;      /* let it drain      */
            uno_snd_mus_stop();
            return;
        }
        uno_snd_stream_write(g_pcm, got);
        done += got;
        if (got < want) break;
    }
}

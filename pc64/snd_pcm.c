/* ===========================================================================
 * UnoDOS/pc64 - the PCM audio layer: a looping 48 kHz s16 stereo DMA ring
 * (HD Audio or AC'97, whichever is present) fed by one of two sources.
 *
 * SQUARE VOICE - the Sound Manager / UnoSound backend. One square wave, most
 * recent note wins, rendered with a short attack/release ramp (no clicks), so
 * Music / Tracker / Dostris / the chime sound the same on a speakerless
 * modern laptop.
 *
 * SAMPLE STREAM - decoded audio (WAV / MIDI / MP3 / AAC) pushed by the Music
 * player at the decoder's own rate, resampled here to the ring's 48 kHz
 * stereo and queued in a software FIFO. While a stream is open it owns the
 * ring and the square voice is muted.
 *
 * Underruns are benign by construction: the hardware loops the ring, and a
 * ring full of a periodic wave replays seamlessly; silence replays silence.
 * A long blocking operation therefore sustains the current note instead of
 * glitching, and the next poll rewrites the future. The stream path makes the
 * same trade explicitly - a starved FIFO emits silence and the decode catches
 * up on the next tick, rather than stalling the shell to wait for samples.
 * ======================================================================== */
#include "snd_pcm.h"
#include "hdaudio.h"
#include "ac97.h"
#include <stdint.h>

#define LEAD_FRAMES 9600u              /* write ~200 ms ahead of the hardware  */
#define AMP_MAX 12000                  /* ~-8.7 dBFS square at volume 100      */
#define RAMP 24                        /* amp step per frame: ~8 ms att/rel    */

#define OUT_RATE 48000

static short   *g_ring;
static unsigned g_frames;              /* ring size in frames                  */
static unsigned (*g_pos)(void);
static void     (*g_kick)(void);       /* AC'97 LVI chase (0 for HDA)          */
static const char *g_name = "";
static unsigned g_w;                   /* write cursor (frames into the ring)  */

static uint32_t g_phase, g_step;       /* 32-bit phase accumulator             */
static int      g_amp, g_target;       /* current / target amplitude (ramped)  */
static int      g_on;                  /* note held                            */
static int      g_vol = 70;            /* 0..100, the Control Panel slider     */

/* ---- the sample-stream FIFO ----------------------------------------------
 * Power-of-two ring of OUTPUT frames (48 kHz stereo), so the cursors wrap by
 * mask. ~0.68 s of buffering: deep enough that a decode burst or a file read
 * never starves the DAC, shallow enough that Stop/seek feels immediate. */
#define FIFO_FRAMES 32768u
#define FIFO_MASK   (FIFO_FRAMES - 1u)
static short    g_fifo[FIFO_FRAMES * 2];
static unsigned g_fr, g_fw;            /* FIFO read / write cursors            */
static int      g_stream;              /* 1 = a stream owns the ring           */
static int      g_paused;
static uint32_t g_rs_ph, g_rs_step;    /* 16.16 resampler phase / step         */
static int      g_rs_pl, g_rs_pr;      /* previous input frame (interpolation) */
static int      g_rs_prime;            /* 1 = no previous frame yet            */
static int      g_src_ch = 2;
static long     g_played;              /* output frames handed to the DAC      */
static int      g_peak;                /* |sample| peak since the last read    */

/* midi -> Hz, the same table as the PC-speaker backend (uefi_main.c) */
static int midi_hz(int midi)
{
    static const int kRatio[12] = {     /* round(4096 * 2^(i/12)) */
        4096, 4340, 4598, 4871, 5161, 5468,
        5793, 6137, 6502, 6889, 7298, 7732
    };
    int n = midi - 69, oct = 0, hz;
    while (n < 0)  { n += 12; oct--; }
    while (n >= 12){ n -= 12; oct++; }
    hz = (440 * kRatio[n]) >> 12;
    while (oct > 0) { hz <<= 1; oct--; }
    while (oct < 0) { hz >>= 1; oct++; }
    return hz;
}

void uno_snd_init(void)
{
    if (uno_hda_init()) {
        g_ring = uno_hda_ring(&g_frames);
        g_pos  = uno_hda_pos;  g_kick = 0;
        g_name = "HD Audio";
    } else if (uno_ac97_init()) {
        g_ring = uno_ac97_ring(&g_frames);
        g_pos  = uno_ac97_pos; g_kick = uno_ac97_kick;
        g_name = "AC'97";
    } else
        return;
    g_w = LEAD_FRAMES;                 /* ring starts as .bss silence          */
}

int uno_snd_active(void) { return g_ring != 0; }
const char *uno_snd_name(void) { return g_name; }

void uno_snd_note(int midi)
{
    int hz = midi_hz(midi);
    if (hz < 20) return;
    g_step   = (uint32_t)(((uint64_t)hz << 32) / OUT_RATE);
    g_on     = 1;
    g_target = AMP_MAX * g_vol / 100;
}

void uno_snd_quiet(void) { g_on = 0; g_target = 0; }

void uno_snd_volume(int pct)
{
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    g_vol = pct;
    if (g_on) g_target = AMP_MAX * g_vol / 100;   /* live while a note plays */
}

/* ---- sample stream -------------------------------------------------------- */

static unsigned fifo_queued(void) { return (g_fw - g_fr) & FIFO_MASK; }
static unsigned fifo_free(void)   { return FIFO_FRAMES - 1u - fifo_queued(); }

void uno_snd_stream_begin(int rate, int channels)
{
    if (rate < 4000)   rate = 4000;            /* keep the step sane           */
    if (rate > 192000) rate = 192000;
    g_src_ch  = (channels >= 2) ? 2 : 1;
    g_rs_step = (uint32_t)(((uint64_t)rate << 16) / OUT_RATE);
    if (!g_rs_step) g_rs_step = 1;
    g_rs_ph   = 0;
    g_rs_pl   = g_rs_pr = 0;
    g_rs_prime = 1;
    g_fr = g_fw = 0;
    g_played = 0;
    g_peak   = 0;
    g_paused = 0;
    g_stream = 1;
    uno_snd_quiet();                           /* the square voice steps aside */
}

void uno_snd_stream_end(void)
{
    g_stream = 0;
    g_paused = 0;
    g_fr = g_fw = 0;
}

int uno_snd_stream_open(void)   { return g_stream; }
int uno_snd_stream_paused(void) { return g_paused; }
void uno_snd_stream_pause(int p) { g_paused = p ? 1 : 0; }
int uno_snd_stream_queued(void) { return (int)fifo_queued(); }
long uno_snd_stream_played(void) { return g_played; }

void uno_snd_stream_flush(void)
{
    g_fr = g_fw = 0;
    g_rs_ph = 0;
    g_rs_prime = 1;
}

int uno_snd_stream_level(void)
{
    int v = g_peak;
    g_peak = 0;
    v = v * 100 / 32767;
    return v > 100 ? 100 : v;
}

/* worst-case output frames one input frame can expand to */
static unsigned rs_expansion(void)
{
    return (0x10000u + g_rs_step - 1u) / g_rs_step + 1u;
}

int uno_snd_stream_space(void)
{
    unsigned free_out, in;
    if (!g_stream) return 0;
    free_out = fifo_free();
    if (free_out < rs_expansion()) return 0;
    /* input frames that fit: free_out * (step / 65536), less a small margin */
    in = (unsigned)(((uint64_t)free_out * g_rs_step) >> 16);
    return in > 2 ? (int)(in - 2) : 0;
}

static void fifo_push(int l, int r)
{
    if (l >  32767) l =  32767; else if (l < -32768) l = -32768;
    if (r >  32767) r =  32767; else if (r < -32768) r = -32768;
    g_fifo[g_fw * 2]     = (short)l;
    g_fifo[g_fw * 2 + 1] = (short)r;
    g_fw = (g_fw + 1u) & FIFO_MASK;
}

int uno_snd_stream_write(const short *pcm, int nframes)
{
    int i;
    unsigned need = rs_expansion();
    if (!g_stream || !pcm || nframes <= 0) return 0;
    for (i = 0; i < nframes; i++) {
        int l, r;
        if (fifo_free() < need) break;         /* caller retries next tick     */
        if (g_src_ch == 2) { l = pcm[i * 2]; r = pcm[i * 2 + 1]; }
        else               { l = r = pcm[i]; }
        if (g_rs_prime) { g_rs_pl = l; g_rs_pr = r; g_rs_prime = 0; }
        /* linear interpolation between the previous input frame and this one */
        while (g_rs_ph < 0x10000u) {
            uint32_t f = g_rs_ph;
            int ol = g_rs_pl + (int)(((int64_t)(l - g_rs_pl) * (int)f) >> 16);
            int or_ = g_rs_pr + (int)(((int64_t)(r - g_rs_pr) * (int)f) >> 16);
            fifo_push(ol, or_);
            g_rs_ph += g_rs_step;
        }
        g_rs_ph -= 0x10000u;
        g_rs_pl = l; g_rs_pr = r;
    }
    return i;
}

/* ---- the ring pump -------------------------------------------------------- */

void uno_snd_poll(void)
{
    unsigned rd, target, n, i;
    if (!g_ring) return;
    rd     = g_pos() % g_frames;
    target = (rd + LEAD_FRAMES) % g_frames;
    /* After a long stall the hardware can lap the write cursor.  The cursor
       is normally 0..LEAD_FRAMES ahead of the read position; more than that
       means DMA overtook it and the "future" we would extend is already the
       past.  Jump just ahead of the hardware so recovery is one clean glitch
       instead of a sustained mangled region. */
    if ((g_w - rd + g_frames) % g_frames > LEAD_FRAMES)
        g_w = (rd + 64) % g_frames;
    n      = (target - g_w + g_frames) % g_frames;
    if (n > g_frames - 64) n = 0;      /* already at the lead target           */

    if (g_stream) {
        /* Sample stream owns the ring. A starved FIFO writes silence: the
           decode catches up next tick instead of blocking the shell here. */
        for (i = 0; i < n; i++) {
            int l = 0, r = 0;
            if (!g_paused && g_fr != g_fw) {
                l = g_fifo[g_fr * 2];
                r = g_fifo[g_fr * 2 + 1];
                g_fr = (g_fr + 1u) & FIFO_MASK;
                g_played++;
                { int a = l < 0 ? -l : l; if (a > g_peak) g_peak = a; }
                l = l * g_vol / 100;
                r = r * g_vol / 100;
            }
            g_ring[g_w * 2]     = (short)l;
            g_ring[g_w * 2 + 1] = (short)r;
            g_w = (g_w + 1) % g_frames;
        }
    } else {
        for (i = 0; i < n; i++) {
            short s;
            if (g_amp < g_target)      { g_amp += RAMP; if (g_amp > g_target) g_amp = g_target; }
            else if (g_amp > g_target) { g_amp -= RAMP; if (g_amp < g_target) g_amp = g_target; }
            s = (g_phase & 0x80000000u) ? (short)g_amp : (short)-g_amp;
            g_phase += g_step;
            g_ring[g_w * 2]     = s;
            g_ring[g_w * 2 + 1] = s;
            g_w = (g_w + 1) % g_frames;
        }
    }
    if (n)                             /* drain stores before DMA reads them   */
        __asm__ volatile ("sfence" ::: "memory");
    if (g_kick) g_kick();
}

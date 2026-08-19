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
 * EFFECTS VOICES - short 8-bit samples held by slot, summed on top of
 * whichever of the two above is playing. They deliberately do NOT take the
 * ring: a game fires a gun while its own score is streaming, and "one source
 * at a time" would cut the music off on every shot.
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
#include <stdlib.h>
#include <string.h>

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
static int      g_owner;               /* UNO_SND_OWN_* of whoever opened it   */
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
{ uno_snd_stream_begin_owned(UNO_SND_OWN_MEDIA, rate, channels); }

int uno_snd_stream_owner(void) { return g_owner; }

void uno_snd_stream_begin_owned(int owner, int rate, int channels)
{
    g_owner = owner;
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

/* ---- the effects voices ---------------------------------------------------
 * A slot holds one sample, kept in the caller's own 8-bit form: 500 KB of
 * Doom effects stay 500 KB instead of becoming a megabyte of s16, and the
 * conversion is two instructions in the mix loop either way.
 *
 * The bank is capped and evicts the least recently PLAYED slot, which the
 * contract in snd_pcm.h allows because reloading a dropped slot costs the
 * caller one copy. A voice is checked against its slot every frame rather
 * than holding a pointer, so an eviction under a sounding voice ends the
 * voice instead of reading freed memory. */
#define SFX_VOICES  8
#define SFX_BUDGET  (2u * 1024 * 1024)     /* all slots together              */

typedef struct {
    unsigned char *pcm;
    unsigned       len;                    /* samples                         */
    uint32_t       step;                   /* 16.16 advance per OUT_RATE frame*/
    unsigned       used;                   /* g_sfx_clock at the last play     */
} sfx_slot;

typedef struct {
    int      slot;
    uint32_t ph;                           /* 16.16 position into the sample  */
    int      gl, gr;                        /* 0..255 per-channel gain         */
    int      on;
} sfx_voice;

static sfx_slot  g_sfx[UNO_SFX_SLOTS];
static sfx_voice g_sv[SFX_VOICES];
static unsigned  g_sfx_bytes;              /* total held by the bank          */
static unsigned  g_sfx_clock;              /* monotonic, for the LRU          */
static int       g_sfx_on;                 /* voices currently sounding       */

static void sfx_kill_slot(int slot)        /* free it, and any voice using it */
{
    int i;
    for (i = 0; i < SFX_VOICES; i++)
        if (g_sv[i].on && g_sv[i].slot == slot) { g_sv[i].on = 0; g_sfx_on--; }
    if (g_sfx[slot].pcm) {
        g_sfx_bytes -= g_sfx[slot].len;
        free(g_sfx[slot].pcm);
    }
    g_sfx[slot].pcm = 0;
    g_sfx[slot].len = 0;
}

int uno_snd_sfx_load(int slot, const unsigned char *pcm, int nsamples, int rate)
{
    unsigned char *copy;
    if (slot < 0 || slot >= UNO_SFX_SLOTS || !pcm || nsamples <= 0) return 0;
    if (rate < 4000)  rate = 4000;
    if (rate > 48000) rate = 48000;
    if ((unsigned)nsamples > SFX_BUDGET) return 0;    /* one sample, the lot   */

    sfx_kill_slot(slot);
    /* make room: drop least recently played first, never the slot being
       loaded (it is already empty) */
    while (g_sfx_bytes + (unsigned)nsamples > SFX_BUDGET) {
        int i, victim = -1;
        for (i = 0; i < UNO_SFX_SLOTS; i++)
            if (g_sfx[i].pcm &&
                (victim < 0 || g_sfx[i].used < g_sfx[victim].used)) victim = i;
        if (victim < 0) break;                        /* nothing left to drop */
        sfx_kill_slot(victim);
    }
    copy = (unsigned char *)malloc((unsigned long)nsamples);
    if (!copy) return 0;
    memcpy(copy, pcm, (unsigned long)nsamples);
    g_sfx[slot].pcm  = copy;
    g_sfx[slot].len  = (unsigned)nsamples;
    g_sfx[slot].step = (uint32_t)(((uint64_t)rate << 16) / OUT_RATE);
    if (!g_sfx[slot].step) g_sfx[slot].step = 1;
    g_sfx[slot].used = ++g_sfx_clock;
    g_sfx_bytes += (unsigned)nsamples;
    return 1;
}

int uno_snd_sfx_play(int slot, int vol, int sep)
{
    int i, v = -1;
    if (slot < 0 || slot >= UNO_SFX_SLOTS || !g_sfx[slot].pcm) return 0;
    if (!g_ring) return 0;             /* no DAC: do not queue voices forever */
    if (vol <= 0) return 0;
    if (vol > 255) vol = 255;
    if (sep < 0) sep = 0; else if (sep > 255) sep = 255;

    for (i = 0; i < SFX_VOICES; i++) if (!g_sv[i].on) { v = i; break; }
    if (v < 0) {                       /* all busy: steal the furthest along  */
        uint32_t best = 0;
        for (i = 0; i < SFX_VOICES; i++)
            if (g_sv[i].ph >= best) { best = g_sv[i].ph; v = i; }
        g_sv[v].on = 0; g_sfx_on--;
    }
    /* Doom's pan law: both channels full at centre, the far channel fading to
       silence at the edge. Deriving it from sep rather than halving both is
       what keeps a centred sound as loud as an unpanned one. */
    { int l = 2 * (255 - sep), r = 2 * sep;
      if (l > 255) l = 255;
      if (r > 255) r = 255;
      g_sv[v].gl = vol * l / 255;
      g_sv[v].gr = vol * r / 255; }
    g_sv[v].slot = slot;
    g_sv[v].ph   = 0;
    g_sv[v].on   = 1;
    g_sfx_on++;
    g_sfx[slot].used = ++g_sfx_clock;
    return 1;
}

void uno_snd_sfx_stop_all(void)
{
    int i;
    for (i = 0; i < SFX_VOICES; i++) g_sv[i].on = 0;
    g_sfx_on = 0;
}

void uno_snd_sfx_free_all(void)
{
    int i;
    uno_snd_sfx_stop_all();
    for (i = 0; i < UNO_SFX_SLOTS; i++) sfx_kill_slot(i);
    g_sfx_bytes = 0;
}

int uno_snd_sfx_playing(void) { return g_sfx_on; }

/* one output frame of every sounding voice, summed into *l / *r */
static void sfx_frame(int *l, int *r)
{
    int i;
    for (i = 0; i < SFX_VOICES; i++) {
        sfx_voice *v = &g_sv[i];
        const unsigned char *p;
        unsigned idx;
        int a, b, s;
        if (!v->on) continue;
        p   = g_sfx[v->slot].pcm;
        idx = v->ph >> 16;
        if (!p || idx + 1 >= g_sfx[v->slot].len) { v->on = 0; g_sfx_on--; continue; }
        /* u8 centred at 128 -> s16. MULTIPLY, do not shift: the value is
           negative for half of every waveform, and left-shifting a negative
           int is undefined - which the debug build's UBSan traps, so the
           first sample played took the guest down with a #UD. */
        a = ((int)p[idx]     - 128) * 256;
        b = ((int)p[idx + 1] - 128) * 256;
        s = a + (int)(((int64_t)(b - a) * (int)(v->ph & 0xFFFF)) >> 16);
        v->ph += g_sfx[v->slot].step;
        *l += (s * v->gl) >> 9;                /* >>9: full volume is half    */
        *r += (s * v->gr) >> 9;                /* scale, so 8 voices can sum  */
    }
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
            if (g_sfx_on) {
                int sl = 0, sr = 0;
                sfx_frame(&sl, &sr);
                l += sl * g_vol / 100;
                r += sr * g_vol / 100;
                if (l >  32767) l =  32767; else if (l < -32768) l = -32768;
                if (r >  32767) r =  32767; else if (r < -32768) r = -32768;
            }
            g_ring[g_w * 2]     = (short)l;
            g_ring[g_w * 2 + 1] = (short)r;
            g_w = (g_w + 1) % g_frames;
        }
    } else {
        for (i = 0; i < n; i++) {
            int l, r;
            if (g_amp < g_target)      { g_amp += RAMP; if (g_amp > g_target) g_amp = g_target; }
            else if (g_amp > g_target) { g_amp -= RAMP; if (g_amp < g_target) g_amp = g_target; }
            l = r = (g_phase & 0x80000000u) ? g_amp : -g_amp;
            g_phase += g_step;
            if (g_sfx_on) {
                int sl = 0, sr = 0;
                sfx_frame(&sl, &sr);
                l += sl * g_vol / 100;
                r += sr * g_vol / 100;
                if (l >  32767) l =  32767; else if (l < -32768) l = -32768;
                if (r >  32767) r =  32767; else if (r < -32768) r = -32768;
            }
            g_ring[g_w * 2]     = (short)l;
            g_ring[g_w * 2 + 1] = (short)r;
            g_w = (g_w + 1) % g_frames;
        }
    }
    if (n)                             /* drain stores before DMA reads them   */
        __asm__ volatile ("sfence" ::: "memory");
    if (g_kick) g_kick();
}

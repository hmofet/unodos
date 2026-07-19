/* ===========================================================================
 * UnoDOS/pc64 - the PCM audio layer: one synthesised voice into a looping
 * 48 kHz s16 stereo DMA ring (HD Audio or AC'97, whichever is present).
 *
 * The voice keeps the PC-speaker semantics the Sound Manager and UnoSound
 * already speak - one square wave, most recent note wins - but rendered as
 * samples with a short attack/release ramp (no clicks), so Music / Tracker /
 * Dostris / the chime sound the same on a speakerless modern laptop.
 *
 * Underruns are benign by construction: the hardware loops the ring, and a
 * ring full of a periodic wave replays seamlessly; silence replays silence.
 * A long blocking operation therefore sustains the current note instead of
 * glitching, and the next poll rewrites the future.
 * ======================================================================== */
#include "snd_pcm.h"
#include "hdaudio.h"
#include "ac97.h"
#include <stdint.h>

#define LEAD_FRAMES 9600u              /* write ~200 ms ahead of the hardware  */
#define AMP_MAX 12000                  /* ~-8.7 dBFS square at volume 100      */
#define RAMP 24                        /* amp step per frame: ~8 ms att/rel    */

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
    g_step   = (uint32_t)(((uint64_t)hz << 32) / 48000u);
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
    if (n)                             /* drain stores before DMA reads them   */
        __asm__ volatile ("sfence" ::: "memory");
    if (g_kick) g_kick();
}

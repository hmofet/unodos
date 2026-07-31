/* UnoAmp output plugins: the machine's sinks, behind Winamp's Out_Module.
 *
 * Three ship built in. They are ordinary unoamp_out records registered at
 * init, identical in kind to a dropped-in OUT_*.UNO, so the built-ins are not
 * a privileged path - the same rule unodevices follows for drivers.
 *
 * WHAT THIS FIXES. uno_snd_stream_begin() never checked that a PCM device
 * exists: with neither HDA nor AC'97 found, g_ring is NULL and
 * uno_snd_active() is 0, but the old player still started the transport, ran
 * the level meter and reported progress - into nothing. Here a sink has to
 * PROBE before it is selected and has to declare CAPS, so a machine with no
 * DAC selects the speaker, the speaker does not claim UNOAMP_CAP_PCM, and the
 * core declines the file instead of pretending.
 */
#include "unoamp.h"
#include "snd_pcm.h"

/* ---- registry -------------------------------------------------------------
 * Probe ORDER is registration order, and it is the plan's "best available
 * hardware, degrading gracefully": real DACs first, the speaker last because
 * it can always be had and would otherwise win every time. */
#define UNOAMP_OUT_MAX 8
static const unoamp_out *g_out[UNOAMP_OUT_MAX];
static int g_nout;
static const unoamp_out *g_sel;
static int g_probed;

int unoamp_register_out(const unoamp_out *o)
{
    int i;
    if (!o || o->version != UNOAMP_ABI || !o->Probe || !o->Write) return 0;
    for (i = 0; i < g_nout; i++) if (g_out[i] == o) return 0;
    if (g_nout >= UNOAMP_OUT_MAX) return 0;
    g_out[g_nout++] = o;
    g_probed = 0;                      /* a new sink can beat the current pick */
    return 1;
}

int unoamp_out_count(void) { return g_nout; }
const unoamp_out *unoamp_out_at(int i)
{ return (i >= 0 && i < g_nout) ? g_out[i] : 0; }

const unoamp_out *unoamp_select_out(void)
{
    int i;
    g_probed = 1;
    g_sel = 0;
    for (i = 0; i < g_nout; i++)
        if (g_out[i]->Probe && g_out[i]->Probe()) { g_sel = g_out[i]; break; }
    return g_sel;
}

const unoamp_out *unoamp_current_out(void)
{
    if (!g_probed) unoamp_select_out();
    return g_sel;
}

unsigned unoamp_caps(void)
{
    const unoamp_out *o = unoamp_current_out();
    return o ? o->caps : 0u;
}

/* ===========================================================================
 * OUT_PCM - HD Audio / AC'97, through the existing snd_pcm ring.
 *
 * snd_pcm already owns the probe (HDA then AC'97), the 48 kHz stereo DMA ring
 * and the resampler, and all of that is metal-proven. This plugin is a shim
 * onto it rather than a rewrite: the value here is the CONTRACT, not new
 * register poking, and rewriting a working audio path to gain an abstraction
 * would be the wrong trade.
 * ======================================================================== */
static int pcm_probe(void) { return uno_snd_active(); }

/* Open records what it opened. snd_pcm keeps the channel count and source rate
 * private, and Winamp's Write/CanWrite are in BYTES while the stream layer
 * counts input FRAMES, so the conversion needs both - and the plugin is the
 * one place that already knows them. */
static int g_ch = 2, g_rate = 48000;
static long g_wr_frames;                /* input frames accepted, for wrtime */

static int pcm_open(int rate, int ch, int bits, int buflen_ms, int pre_ms)
{
    (void)buflen_ms; (void)pre_ms;
    if (bits != 16) return -1;                 /* the ring is s16 throughout  */
    if (!uno_snd_active()) return -1;
    g_ch = (ch >= 2) ? 2 : 1;
    g_rate = rate > 0 ? rate : 48000;
    g_wr_frames = 0;
    uno_snd_stream_begin(rate, ch);
    return 0;
}
static void pcm_close(void) { uno_snd_stream_end(); }

static int pcm_frame_bytes(void) { return g_ch * 2; }

static int pcm_write(const char *buf, int len)
{
    int fb = pcm_frame_bytes();
    int frames = len / fb, got;
    if (frames <= 0) return 0;
    got = uno_snd_stream_write((const short *)buf, frames);
    g_wr_frames += got;
    return got * fb;
}
static int pcm_canwrite(void)  { return uno_snd_stream_space() * pcm_frame_bytes(); }
static int pcm_isplaying(void) { return uno_snd_stream_open() && uno_snd_stream_queued() > 0; }

/* Winamp's Pause returns the PREVIOUS state; snd_pcm's setter returns void, so
 * read it first rather than inventing a return value. */
static int pcm_pause(int p)
{
    int prev = uno_snd_stream_paused();
    uno_snd_stream_pause(p);
    return prev;
}
static void pcm_setvol(int v)  { uno_snd_volume(v * 100 / 255); }
static void pcm_setpan(int p)  { (void)p; }       /* the ring is centre-only  */

/* Winamp passes the post-flush timestamp; snd_pcm's flush takes none and the
 * player re-seeks the decoder itself, so the argument is accepted and ignored
 * rather than faked. */
static void pcm_flush(int ms)  { (void)ms; uno_snd_stream_flush(); }

/* played() counts OUTPUT frames, and the ring is 48 kHz by construction
 * (snd_pcm.h: "a 48 kHz s16 stereo DMA ring"). Written time is in the SOURCE
 * rate, because that is the clock the decoder is feeding. */
#define PCM_OUT_HZ 48000
static int  pcm_outtime(void)
{ return (int)(uno_snd_stream_played() * 1000 / PCM_OUT_HZ); }
static int  pcm_wrtime(void)
{ return g_rate > 0 ? (int)(g_wr_frames * 1000 / g_rate) : 0; }

static const unoamp_out g_out_pcm = {
    UNOAMP_ABI, "PCM (HD Audio / AC'97)", 1, 0, 0,
    UNOAMP_CAP_PCM | UNOAMP_CAP_SEEK,
    0, 0, 0, 0,                                    /* Config/About/Init/Quit  */
    pcm_probe, pcm_open, pcm_close, pcm_write, pcm_canwrite, pcm_isplaying,
    pcm_pause, pcm_setvol, pcm_setpan, pcm_flush, pcm_outtime, pcm_wrtime
};

/* ===========================================================================
 * OUT_SPEAKER - the PIT square voice.
 *
 * It advertises SQUARE and NOT PCM, which is the whole point: it can carry the
 * built-in tune library and a MIDI melody, and it cannot carry a WAV. Saying
 * so is what lets the core refuse honestly.
 *
 * The plan's 1-bit PWM idea - bit-banging sampled audio through the speaker -
 * would upgrade this to CAP_PCM. It is deliberately NOT here: it needs a
 * high-rate timer interrupt driving port 0x61 in real time, which on a
 * cooperative frame loop is a scheduling change rather than an audio feature,
 * and it is unverifiable in QEMU by anything but ear. Its own slice.
 * ======================================================================== */
static int spk_probe(void) { return 1; }           /* the fallback that always
                                                      answers; last in order  */
static int spk_open(int rate, int ch, int bits, int b, int p)
{ (void)rate; (void)ch; (void)bits; (void)b; (void)p; return -1; }
static void spk_close(void) { uno_snd_quiet(); }
static int  spk_write(const char *buf, int len) { (void)buf; (void)len; return 0; }
static int  spk_canwrite(void)  { return 0; }
static int  spk_isplaying(void) { return 0; }
static int  spk_pause(int p)    { (void)p; uno_snd_quiet(); return 0; }
static void spk_setvol(int v)   { uno_snd_volume(v * 100 / 255); }
static void spk_setpan(int p)   { (void)p; }
static void spk_flush(int ms)   { (void)ms; uno_snd_quiet(); }
static int  spk_outtime(void)   { return 0; }
static int  spk_wrtime(void)    { return 0; }

static const unoamp_out g_out_spk = {
    UNOAMP_ABI, "PC speaker", 2, 0, 0,
    UNOAMP_CAP_SQUARE,
    0, 0, 0, 0,
    spk_probe, spk_open, spk_close, spk_write, spk_canwrite, spk_isplaying,
    spk_pause, spk_setvol, spk_setpan, spk_flush, spk_outtime, spk_wrtime
};

/* ---- built-in registration ------------------------------------------------ */
void unoamp_out_init(void)
{
    unoamp_register_out(&g_out_pcm);    /* real DAC first  */
    unoamp_register_out(&g_out_spk);    /* fallback last   */
    unoamp_select_out();
}

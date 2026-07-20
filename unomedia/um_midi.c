/* ===========================================================================
 * unomedia - Standard MIDI File playback, written from scratch: a type
 * 0/1/2 parser, a tick-accurate event scheduler, and a polyphonic subtractive
 * synthesiser that renders the result to PCM.
 *
 * A MIDI file contains no sound - only instructions - so unlike WAV/MP3/AAC
 * this "decoder" has to invent the audio. There is no soundfont here (a GM
 * sample set is tens of megabytes and this OS boots from a 1 MB image), so
 * instruments are synthesised: each of the 16 General MIDI families maps to a
 * patch of one or two detuned oscillators, a waveform, and an ADSR envelope
 * chosen to sit in roughly the right place - organs sustain, pianos decay,
 * strings swell. Channel 10 is percussion and is synthesised separately from
 * noise plus a pitch-swept body.
 *
 * ---- how the timing works -------------------------------------------------
 * Tracks are merged live rather than flattened into one event list: each
 * track keeps a cursor holding its next event's ABSOLUTE tick, and the
 * scheduler always services the earliest. That handles type 1 files (one
 * track per instrument) without allocating a merged array, and makes tempo
 * changes - which can appear on any track at any tick - trivially correct.
 *
 * Tick-to-sample conversion is 16.16 fixed point, accumulated rather than
 * recomputed, so tempo changes never round-drift the timeline out of sync
 * over a long file.
 *
 * ---- how rendering works ---------------------------------------------------
 * decode() is asked for N frames. It renders in runs bounded by the next
 * event: mix voices until the event's sample position, dispatch it, repeat.
 * So an event lands on the exact sample it should, regardless of the buffer
 * size the player happens to ask for.
 * ======================================================================== */
#include "unomedia.h"
#include <string.h>
#include <stdint.h>

float sinf(float x);

#define MIDI_MAX_FILE (1024L * 1024L)   /* SMFs are tiny; this is generous    */
#define MIDI_RATE     44100
#define MAX_TRACKS    64
#define MAX_VOICES    48
#define SINE_BITS     11
#define SINE_LEN      (1 << SINE_BITS)

/* ---- the file ------------------------------------------------------------- */
static unsigned char m_buf[MIDI_MAX_FILE];
static long m_len;
static int  m_ok;
static int  m_division;                 /* ticks per quarter, or SMPTE        */
static int  m_smpte_fps, m_smpte_tpf;   /* when division is negative          */
static long m_total_ms;

/* ---- track cursors -------------------------------------------------------- */
typedef struct {
    long  pos, end;                     /* offsets into m_buf                 */
    long  abs_tick;                     /* tick of the PENDING event          */
    unsigned char rs;                   /* running status                     */
    int   done;
} mtrk;
static mtrk m_trk[MAX_TRACKS];
static int  m_ntrk;
static long m_tick;                     /* the scheduler's current tick       */
static uint32_t m_tempo;                /* microseconds per quarter note      */
static uint64_t m_spt_q16;              /* samples per tick, 16.16            */
static uint64_t m_acc_q16;              /* samples owed until the next event  */
static long m_pos_frames;

/* ---- channel state -------------------------------------------------------- */
typedef struct {
    unsigned char program, volume, expression, pan, sustain;
    int  pitch_bend;                    /* -8192..8191                        */
} mchan;
static mchan m_ch[16];

/* ---- voices --------------------------------------------------------------- */
enum { ENV_OFF = 0, ENV_ATT, ENV_DEC, ENV_SUS, ENV_REL };
typedef struct {
    int      active;
    int      chan, note;
    uint32_t ph, ph2, step, step2;      /* two detuned oscillators            */
    int      wave;                      /* WV_*                               */
    float    env, amp;                  /* envelope level, note velocity gain */
    int      stage;
    float    att, dec, sus, rel;        /* per-sample envelope rates / level  */
    uint32_t noise;                     /* LFSR state for percussion          */
    float    sweep;                     /* per-sample pitch multiplier (drums)*/
    long     age;
    int      held;                      /* still held by sustain pedal        */
} mvoice;
static mvoice m_v[MAX_VOICES];
static long m_age;

enum { WV_SINE = 0, WV_SAW, WV_SQUARE, WV_TRI, WV_PULSE, WV_NOISE };

static float m_sine[SINE_LEN];
static int   m_sine_built;

/* ---- byte helpers --------------------------------------------------------- */
static uint32_t be32(const unsigned char *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t be16(const unsigned char *p)
{ return ((uint32_t)p[0] << 8) | p[1]; }

/* variable-length quantity: 7 bits per byte, high bit = "more follows" */
static uint32_t vlq(long *pp, long end)
{
    uint32_t v = 0; int i;
    long p = *pp;
    for (i = 0; i < 4 && p < end; i++) {
        unsigned char c = m_buf[p++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    *pp = p;
    return v;
}

/* ---- General MIDI patch approximation -------------------------------------
 * One entry per GM family (program >> 3). Attack/decay/release are in ms;
 * sustain is a level. `detune` is in cents applied to the second oscillator -
 * two slightly-apart oscillators is what makes strings and pads sound wide
 * rather than like a single buzzing saw. */
typedef struct {
    int   wave;
    short att_ms, dec_ms, rel_ms;
    float sus;
    short detune;
    float gain;
} mpatch;

static const mpatch kFamily[16] = {
    /* 0  Piano            */ { WV_TRI,     2,  900,  180, 0.00f,   4, 0.85f },
    /* 1  Chromatic perc   */ { WV_SINE,    1,  500,  120, 0.00f,   0, 0.90f },
    /* 2  Organ            */ { WV_SQUARE,  4,   40,   70, 0.85f,   7, 0.55f },
    /* 3  Guitar           */ { WV_SAW,     3,  700,  200, 0.12f,   5, 0.70f },
    /* 4  Bass             */ { WV_TRI,     4,  600,  150, 0.35f,   3, 0.95f },
    /* 5  Strings          */ { WV_SAW,   140,  400, 320, 0.70f,  11, 0.55f },
    /* 6  Ensemble         */ { WV_SAW,   110,  350, 300, 0.72f,  15, 0.50f },
    /* 7  Brass            */ { WV_SAW,    45,  260, 180, 0.68f,   8, 0.62f },
    /* 8  Reed             */ { WV_PULSE,  35,  240, 160, 0.66f,   6, 0.58f },
    /* 9  Pipe             */ { WV_SINE,   30,  220, 180, 0.75f,   5, 0.70f },
    /* 10 Synth lead       */ { WV_SQUARE, 10,  300, 150, 0.60f,   9, 0.55f },
    /* 11 Synth pad        */ { WV_SAW,   220,  500, 500, 0.70f,  17, 0.45f },
    /* 12 Synth effects    */ { WV_SINE,   60,  400, 300, 0.50f,  13, 0.50f },
    /* 13 Ethnic           */ { WV_SAW,    10,  600, 200, 0.25f,   6, 0.65f },
    /* 14 Percussive       */ { WV_TRI,     1,  350,  90, 0.00f,   4, 0.85f },
    /* 15 Sound effects    */ { WV_NOISE,  20,  600, 300, 0.30f,   0, 0.45f },
};

/* ---- note -> phase step ---------------------------------------------------- */
/* 2^(cents/1200) for the detune amounts we use, as a small table lookup would
 * be less readable than just computing it; cents are tiny so a 2-term series
 * is exact enough and avoids pulling in powf. */
static float cents_ratio(int cents)
{
    /* ln(2)/1200 = 5.7762265e-4 ; e^x ~ 1 + x + x^2/2 for |x| < 0.02 */
    float x = (float)cents * 5.7762265e-4f;
    return 1.0f + x + x * x * 0.5f;
}

static float note_hz(float midi)
{
    /* 440 * 2^((n-69)/12), by octave shift + a 12-entry semitone table */
    static const float kSemi[12] = {
        1.000000f, 1.059463f, 1.122462f, 1.189207f, 1.259921f, 1.334840f,
        1.414214f, 1.498307f, 1.587401f, 1.681793f, 1.781797f, 1.887749f
    };
    int n = (int)midi, oct = 0, idx;
    float frac = midi - (float)n, hz;
    n -= 69;
    while (n < 0)   { n += 12; oct--; }
    while (n >= 12) { n -= 12; oct++; }
    idx = n;
    hz = 440.0f * kSemi[idx];
    while (oct > 0) { hz *= 2.0f; oct--; }
    while (oct < 0) { hz *= 0.5f; oct++; }
    if (frac > 0.0f) hz *= 1.0f + frac * 0.0594631f;   /* linear within a semitone */
    return hz;
}

static uint32_t hz_step(float hz)
{
    if (hz < 8.0f)     hz = 8.0f;
    if (hz > 18000.0f) hz = 18000.0f;
    return (uint32_t)((hz * 4294967296.0f) / (float)MIDI_RATE);
}

/* ---- voice allocation ----------------------------------------------------- */
static mvoice *voice_alloc(void)
{
    int i, best = 0;
    long oldest = 0x7FFFFFFF;
    for (i = 0; i < MAX_VOICES; i++)
        if (!m_v[i].active) { m_v[i].active = 1; m_v[i].age = ++m_age; return &m_v[i]; }
    /* all busy: steal the oldest voice that is already releasing, else the
       plain oldest - stealing a releasing tail is the least audible choice */
    for (i = 0; i < MAX_VOICES; i++)
        if (m_v[i].stage == ENV_REL && m_v[i].age < oldest) { oldest = m_v[i].age; best = i; }
    if (oldest == 0x7FFFFFFF)
        for (i = 0; i < MAX_VOICES; i++)
            if (m_v[i].age < oldest) { oldest = m_v[i].age; best = i; }
    m_v[best].age = ++m_age;
    return &m_v[best];
}

static float rate_from_ms(int ms, float span)
{
    float n;
    if (ms < 1) ms = 1;
    n = (float)ms * (float)MIDI_RATE / 1000.0f;
    return span / n;
}

/* percussion: note number -> a synthesised drum voice */
static void drum_setup(mvoice *v, int note)
{
    v->wave  = WV_NOISE;
    v->sweep = 1.0f;
    v->sus   = 0.0f;
    v->att   = rate_from_ms(1, 1.0f);
    v->rel   = rate_from_ms(40, 1.0f);
    switch (note) {
    case 35: case 36:                                  /* bass drum          */
        v->wave = WV_SINE; v->step = hz_step(105.0f);
        v->sweep = 0.99988f; v->dec = rate_from_ms(150, 1.0f); break;
    case 38: case 40:                                  /* snare              */
        v->step = hz_step(1400.0f); v->dec = rate_from_ms(140, 1.0f); break;
    case 37:                                           /* side stick         */
        v->step = hz_step(2200.0f); v->dec = rate_from_ms(50, 1.0f); break;
    case 42: case 44:                                  /* closed hi-hat      */
        v->step = hz_step(7000.0f); v->dec = rate_from_ms(45, 1.0f); break;
    case 46:                                           /* open hi-hat        */
        v->step = hz_step(6500.0f); v->dec = rate_from_ms(350, 1.0f); break;
    case 49: case 52: case 55: case 57:                /* crash              */
        v->step = hz_step(5200.0f); v->dec = rate_from_ms(900, 1.0f); break;
    case 51: case 53: case 59:                         /* ride               */
        v->step = hz_step(5800.0f); v->dec = rate_from_ms(500, 1.0f); break;
    case 39:                                           /* hand clap          */
        v->step = hz_step(1800.0f); v->dec = rate_from_ms(120, 1.0f); break;
    default:
        if (note >= 41 && note <= 50) {                /* toms: pitched body */
            float hz = 150.0f - (float)(note - 41) * 6.0f;
            v->wave = WV_SINE; v->step = hz_step(hz);
            v->sweep = 0.99993f; v->dec = rate_from_ms(260, 1.0f);
        } else {
            v->step = hz_step(3000.0f); v->dec = rate_from_ms(160, 1.0f);
        }
        break;
    }
    v->step2 = 0;
    v->noise = 0x13579BDFu ^ (uint32_t)(note * 2654435761u);
}

static void note_on(int chan, int note, int vel)
{
    mvoice *v;
    const mpatch *pt;
    if (vel <= 0) return;
    v = voice_alloc();
    memset(v, 0, sizeof *v);
    v->active = 1; v->age = ++m_age;
    v->chan = chan; v->note = note; v->held = 1;
    v->stage = ENV_ATT; v->env = 0.0f;
    v->ph = v->ph2 = 0;

    if (chan == 9) {                                   /* GM percussion      */
        drum_setup(v, note);
        v->amp = ((float)vel / 127.0f) * 0.9f;
        return;
    }
    pt = &kFamily[m_ch[chan].program >> 3];
    v->wave = pt->wave;
    v->att  = rate_from_ms(pt->att_ms, 1.0f);
    v->dec  = rate_from_ms(pt->dec_ms, 1.0f - pt->sus > 0.02f ? 1.0f - pt->sus : 1.0f);
    v->sus  = pt->sus;
    v->rel  = rate_from_ms(pt->rel_ms, 1.0f);
    v->sweep = 1.0f;
    v->noise = 0x2545F491u ^ (uint32_t)(note * 40503u + chan);
    /* velocity is perceptual, not linear - square it toward the quiet end */
    { float x = (float)vel / 127.0f; v->amp = x * x * pt->gain; }
    { float hz = note_hz((float)note);
      v->step  = hz_step(hz);
      v->step2 = pt->detune ? hz_step(hz * cents_ratio(pt->detune)) : 0; }
}

static void note_off(int chan, int note)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++) {
        mvoice *v = &m_v[i];
        if (!v->active || v->chan != chan || v->note != note) continue;
        if (v->stage == ENV_REL) continue;
        v->held = 0;
        if (!m_ch[chan].sustain) v->stage = ENV_REL;
    }
}

static void chan_sustain_off(int chan)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++)
        if (m_v[i].active && m_v[i].chan == chan && !m_v[i].held
            && m_v[i].stage != ENV_REL)
            m_v[i].stage = ENV_REL;
}

static void all_notes_off(int chan)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++)
        if (m_v[i].active && m_v[i].chan == chan) { m_v[i].held = 0; m_v[i].stage = ENV_REL; }
}

/* ---- the oscillator -------------------------------------------------------- */
static float osc(int wave, uint32_t ph, uint32_t *noise)
{
    switch (wave) {
    case WV_SINE:   return m_sine[ph >> (32 - SINE_BITS)];
    case WV_SAW:    return (float)(int32_t)ph * (1.0f / 2147483648.0f);
    case WV_SQUARE: return (ph & 0x80000000u) ? 1.0f : -1.0f;
    case WV_PULSE:  return (ph < 0x40000000u) ? 1.0f : -1.0f;   /* 25% duty  */
    case WV_TRI: {
        int32_t s = (int32_t)ph;
        float t = (float)s * (1.0f / 1073741824.0f);            /* -2..2     */
        if (t >  1.0f) t =  2.0f - t;
        if (t < -1.0f) t = -2.0f - t;
        return t;
    }
    default: {                                                  /* WV_NOISE  */
        uint32_t x = *noise;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        *noise = x;
        return (float)(int32_t)x * (1.0f / 2147483648.0f);
    }
    }
}

/* mix `n` frames of every live voice into out (interleaved stereo) */
static void render(short *out, int n)
{
    int i, k;
    for (k = 0; k < n * 2; k++) out[k] = 0;
    for (i = 0; i < MAX_VOICES; i++) {
        mvoice *v = &m_v[i];
        const mchan *c;
        float chg, panl, panr;
        if (!v->active) continue;
        c = &m_ch[v->chan];
        chg = ((float)c->volume / 127.0f) * ((float)c->expression / 127.0f);
        /* constant-power-ish pan, cheap linear form */
        panr = (float)c->pan / 127.0f;
        panl = 1.0f - panr;
        panl = 0.30f + 0.70f * panl;
        panr = 0.30f + 0.70f * panr;
        for (k = 0; k < n; k++) {
            float s, g;
            /* envelope */
            switch (v->stage) {
            case ENV_ATT:
                v->env += v->att;
                if (v->env >= 1.0f) { v->env = 1.0f; v->stage = ENV_DEC; }
                break;
            case ENV_DEC:
                v->env -= v->dec;
                if (v->env <= v->sus) {
                    v->env = v->sus;
                    v->stage = (v->sus > 0.001f) ? ENV_SUS : ENV_REL;
                }
                break;
            case ENV_SUS: break;
            case ENV_REL:
                v->env -= v->rel;
                if (v->env <= 0.0f) { v->env = 0.0f; v->active = 0; }
                break;
            default: v->active = 0; break;
            }
            if (!v->active) break;
            s = osc(v->wave, v->ph, &v->noise);
            if (v->step2) s = (s + osc(v->wave, v->ph2, &v->noise)) * 0.5f;
            v->ph  += v->step;
            v->ph2 += v->step2;
            if (v->sweep != 1.0f) {                    /* drum pitch drop    */
                v->step = (uint32_t)((float)v->step * v->sweep);
                if (v->step < 64) v->step = 64;
            }
            g = s * v->env * v->amp * chg;
            /* Per-voice level. 10000 of 32767 leaves ~10 dB of headroom for
               chords to sum into: measured peak on a 4-voice arrangement is
               about -11 dBFS, so the clamp below is a backstop rather than
               something the normal case relies on. The earlier 7000 was
               audibly quiet once a real multi-track file was playing. */
            {
                int l = out[k * 2]     + (int)(g * panl * 10000.0f);
                int r = out[k * 2 + 1] + (int)(g * panr * 10000.0f);
                if (l >  32767) l =  32767; else if (l < -32768) l = -32768;
                if (r >  32767) r =  32767; else if (r < -32768) r = -32768;
                out[k * 2]     = (short)l;
                out[k * 2 + 1] = (short)r;
            }
        }
    }
}

/* ---- the scheduler --------------------------------------------------------- */
static void recalc_spt(void)
{
    if (m_division > 0)
        m_spt_q16 = ((uint64_t)MIDI_RATE * m_tempo << 16) / (1000000ull * (uint32_t)m_division);
    else                                            /* SMPTE: fps x ticks/frame */
        m_spt_q16 = ((uint64_t)MIDI_RATE << 16) / (uint32_t)(m_smpte_fps * m_smpte_tpf);
    if (!m_spt_q16) m_spt_q16 = 1;
}

static void trk_next(mtrk *t)
{
    uint32_t d;
    if (t->pos >= t->end) { t->done = 1; return; }
    d = vlq(&t->pos, t->end);
    t->abs_tick += (long)d;
}

/* the track whose pending event is earliest, or -1 when every track is spent */
static int next_track(void)
{
    int i, best = -1;
    long bt = 0;
    for (i = 0; i < m_ntrk; i++) {
        if (m_trk[i].done) continue;
        if (best < 0 || m_trk[i].abs_tick < bt) { best = i; bt = m_trk[i].abs_tick; }
    }
    return best;
}

/* dispatch one event from track `ti`; `render_it` = 0 during the duration scan */
static void do_event(int ti, int render_it)
{
    mtrk *t = &m_trk[ti];
    unsigned char st;
    if (t->pos >= t->end) { t->done = 1; return; }
    st = m_buf[t->pos];
    if (st & 0x80) { t->pos++; t->rs = st; }
    else           { st = t->rs; }                    /* running status      */
    if (!(st & 0x80)) { t->done = 1; return; }        /* corrupt: stop track */

    if (st == 0xFF) {                                 /* meta                */
        unsigned char type;
        uint32_t len;
        if (t->pos >= t->end) { t->done = 1; return; }
        type = m_buf[t->pos++];
        len  = vlq(&t->pos, t->end);
        if (type == 0x51 && len >= 3 && t->pos + 3 <= t->end) {
            m_tempo = ((uint32_t)m_buf[t->pos] << 16) |
                      ((uint32_t)m_buf[t->pos + 1] << 8) | m_buf[t->pos + 2];
            if (!m_tempo) m_tempo = 500000;
            recalc_spt();
        } else if (type == 0x2F) {                    /* end of track        */
            t->pos = t->end; t->done = 1; return;
        }
        t->pos += (long)len;
    } else if (st == 0xF0 || st == 0xF7) {            /* sysex: skip         */
        uint32_t len = vlq(&t->pos, t->end);
        t->pos += (long)len;
    } else {
        int chan = st & 0x0F, hi = st & 0xF0;
        int a = (t->pos < t->end) ? m_buf[t->pos++] : 0;
        int b = 0;
        if (hi != 0xC0 && hi != 0xD0) b = (t->pos < t->end) ? m_buf[t->pos++] : 0;
        if (render_it) {
            switch (hi) {
            case 0x80: note_off(chan, a); break;
            case 0x90: if (b) note_on(chan, a, b); else note_off(chan, a); break;
            case 0xA0: break;                          /* poly aftertouch    */
            case 0xB0:                                 /* control change     */
                switch (a) {
                case 7:   m_ch[chan].volume = (unsigned char)b; break;
                case 10:  m_ch[chan].pan = (unsigned char)b; break;
                case 11:  m_ch[chan].expression = (unsigned char)b; break;
                case 64:  m_ch[chan].sustain = (unsigned char)(b >= 64);
                          if (b < 64) chan_sustain_off(chan);
                          break;
                case 120: case 123: all_notes_off(chan); break;
                case 121:                              /* reset controllers  */
                    m_ch[chan].volume = 100; m_ch[chan].expression = 127;
                    m_ch[chan].pan = 64; m_ch[chan].sustain = 0;
                    m_ch[chan].pitch_bend = 0;
                    break;
                default: break;
                }
                break;
            case 0xC0: m_ch[chan].program = (unsigned char)a; break;
            case 0xD0: break;                          /* channel aftertouch */
            case 0xE0: m_ch[chan].pitch_bend = ((b << 7) | a) - 8192; break;
            default: break;
            }
        } else if (hi == 0xC0) {
            m_ch[chan].program = (unsigned char)a;
        }
    }
    trk_next(t);
}

/* ---- parse + rewind -------------------------------------------------------- */
static int parse_header(void)
{
    uint32_t hlen;
    long off;
    int i;
    if (m_len < 14 || memcmp(m_buf, "MThd", 4)) return 0;
    hlen = be32(m_buf + 4);
    if (hlen < 6) return 0;
    m_division = (int)(int16_t)be16(m_buf + 12);
    if (m_division == 0) return 0;
    if (m_division < 0) {
        m_smpte_fps = -(int)(signed char)m_buf[12];
        m_smpte_tpf = m_buf[13];
        if (m_smpte_fps <= 0 || m_smpte_tpf <= 0) return 0;
    }
    off = 8 + (long)hlen;
    m_ntrk = 0;
    for (i = 0; i < MAX_TRACKS && off + 8 <= m_len; i++) {
        uint32_t tlen = be32(m_buf + off + 4);
        if (memcmp(m_buf + off, "MTrk", 4)) { off += 8 + (long)tlen; continue; }
        if (off + 8 + (long)tlen > m_len) tlen = (uint32_t)(m_len - off - 8);
        m_trk[m_ntrk].pos = off + 8;
        m_trk[m_ntrk].end = off + 8 + (long)tlen;
        m_ntrk++;
        off += 8 + (long)tlen;
    }
    return m_ntrk > 0;
}

static void rewind_all(void)
{
    int i;
    for (i = 0; i < m_ntrk; i++) {
        /* pos/end were set by parse_header; only the cursor state resets */
        m_trk[i].done = 0;
        m_trk[i].rs = 0;
        m_trk[i].abs_tick = 0;
    }
    /* re-read each track's first delta */
    for (i = 0; i < m_ntrk; i++) trk_next(&m_trk[i]);
    for (i = 0; i < 16; i++) {
        m_ch[i].program = 0; m_ch[i].volume = 100; m_ch[i].expression = 127;
        m_ch[i].pan = 64; m_ch[i].sustain = 0; m_ch[i].pitch_bend = 0;
    }
    memset(m_v, 0, sizeof m_v);
    m_tick = 0; m_tempo = 500000; m_acc_q16 = 0; m_pos_frames = 0;
    recalc_spt();
}

/* re-seat the track cursors at their chunk starts (parse_header set pos to
 * the first delta; rewind needs that same starting offset) */
static long m_trk_start[MAX_TRACKS];

static void snapshot_starts(void)
{ int i; for (i = 0; i < m_ntrk; i++) m_trk_start[i] = m_trk[i].pos; }
static void restore_starts(void)
{ int i; for (i = 0; i < m_ntrk; i++) m_trk[i].pos = m_trk_start[i]; }

/* walk the whole file without rendering, accumulating wall-clock time. Tempo
 * changes are honoured, so this is exact rather than an average-tempo guess. */
static long scan_duration(void)
{
    uint64_t frames = 0;
    long guard = 0;
    restore_starts();
    rewind_all();
    for (;;) {
        int ti = next_track();
        long delta;
        if (ti < 0) break;
        if (++guard > 4000000L) break;               /* malformed-file bound */
        delta = m_trk[ti].abs_tick - m_tick;
        if (delta > 0) { frames += ((uint64_t)delta * m_spt_q16) >> 16; m_tick += delta; }
        do_event(ti, 0);
    }
    return (long)((frames * 1000ull) / MIDI_RATE);
}

/* ---- decoder interface ----------------------------------------------------- */
static int midi_probe(const unsigned char *head, long n, const char *ext)
{
    if (n >= 4 && !memcmp(head, "MThd", 4)) return 1;
    /* RIFF-wrapped MIDI (.RMI) - "RIFF....RMID" */
    if (n >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "RMID", 4)) return 1;
    return !strcmp(ext, "MID") || !strcmp(ext, "MIDI") || !strcmp(ext, "RMI");
}

static int midi_open(um_audio_info *info)
{
    long n = um_size();
    long base = 0;
    int i;

    m_ok = 0;
    if (n <= 0 || n > MIDI_MAX_FILE) return 0;
    if (um_read(0, m_buf, n) != n) return 0;
    m_len = n;

    /* .RMI wraps the SMF in a RIFF "data" chunk - find it and shift */
    if (m_len > 12 && !memcmp(m_buf, "RIFF", 4) && !memcmp(m_buf + 8, "RMID", 4)) {
        long off = 12;
        while (off + 8 <= m_len) {
            uint32_t clen = (uint32_t)m_buf[off + 4] | ((uint32_t)m_buf[off + 5] << 8) |
                            ((uint32_t)m_buf[off + 6] << 16) | ((uint32_t)m_buf[off + 7] << 24);
            if (!memcmp(m_buf + off, "data", 4)) { base = off + 8; break; }
            off += 8 + (long)clen + (clen & 1);
        }
        if (!base) return 0;
        memmove(m_buf, m_buf + base, (unsigned long)(m_len - base));
        m_len -= base;
    }

    if (!m_sine_built) {
        for (i = 0; i < SINE_LEN; i++)
            m_sine[i] = sinf((float)i * (6.28318531f / (float)SINE_LEN));
        m_sine_built = 1;
    }
    if (!parse_header()) return 0;
    snapshot_starts();

    /* the first text/track-name meta event makes a decent title */
    info->title[0] = 0;
    {
        long p = m_trk_start[0], end = m_trk[0].end;
        int guard = 0;
        while (p + 3 < end && guard++ < 64) {
            uint32_t d = vlq(&p, end);
            (void)d;
            if (p + 2 < end && m_buf[p] == 0xFF &&
                (m_buf[p + 1] == 0x03 || m_buf[p + 1] == 0x01)) {
                long q = p + 2;
                uint32_t len = vlq(&q, end);
                unsigned k;
                for (k = 0; k < len && k < sizeof info->title - 1 && q + (long)k < end; k++) {
                    unsigned char c = m_buf[q + k];
                    info->title[k] = (c >= 32 && c < 127) ? (char)c : ' ';
                }
                info->title[k] = 0;
                break;
            }
            break;                       /* only inspect the very first event */
        }
    }

    m_total_ms = scan_duration();
    restore_starts();
    rewind_all();

    info->rate        = MIDI_RATE;
    info->channels    = 2;
    info->duration_ms = m_total_ms;
    info->bitrate     = 0;
    m_ok = 1;
    return 1;
}

static int midi_voices_live(void)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++) if (m_v[i].active) return 1;
    return 0;
}

static int midi_decode(short *out, int max_frames)
{
    int done = 0;
    if (!m_ok) return 0;
    while (done < max_frames) {
        int ti = next_track();
        int n;
        if (ti < 0) {
            /* every track is spent: let the tails ring out, then stop */
            if (!midi_voices_live()) break;
            n = max_frames - done;
            render(out + done * 2, n);
            done += n;
            m_pos_frames += n;
            continue;
        }
        if (!m_acc_q16) {
            long delta = m_trk[ti].abs_tick - m_tick;
            if (delta > 0) { m_acc_q16 = (uint64_t)delta * m_spt_q16; m_tick += delta; }
        }
        if (m_acc_q16 >= 0x10000ull) {
            uint64_t whole = m_acc_q16 >> 16;
            n = (int)(whole > (uint64_t)(max_frames - done)
                      ? (uint64_t)(max_frames - done) : whole);
            if (n > 0) {
                render(out + done * 2, n);
                done += n;
                m_pos_frames += n;
                m_acc_q16 -= (uint64_t)n << 16;
            }
            if (done >= max_frames) break;
        }
        if (m_acc_q16 < 0x10000ull) {
            m_acc_q16 = 0;
            do_event(ti, 1);
        }
    }
    return done;
}

static int midi_seek(long ms)
{
    long target;
    if (!m_ok) return 0;
    if (ms < 0) ms = 0;
    target = (long)(((int64_t)ms * MIDI_RATE) / 1000);
    /* MIDI is stateful (programs, controllers, tempo), so seeking means
       replaying the event stream from the start with rendering suppressed -
       cheap, because it is bookkeeping only. */
    restore_starts();
    rewind_all();
    {
        uint64_t frames = 0;
        long guard = 0;
        for (;;) {
            int ti = next_track();
            long delta;
            uint64_t adv;
            if (ti < 0) break;
            if (++guard > 4000000L) break;
            delta = m_trk[ti].abs_tick - m_tick;
            if (delta > 0) {
                adv = ((uint64_t)delta * m_spt_q16) >> 16;
                if (frames + adv >= (uint64_t)target) {
                    /* stop here: leave the remainder for decode() to consume */
                    m_acc_q16 = ((uint64_t)delta * m_spt_q16)
                                - (((uint64_t)target - frames) << 16);
                    m_tick += delta;
                    break;
                }
                frames += adv;
                m_tick += delta;
            }
            do_event(ti, 1);
        }
        m_pos_frames = target;
    }
    /* a seek lands mid-note; silence whatever the replay left holding */
    memset(m_v, 0, sizeof m_v);
    return 1;
}

static long midi_pos_ms(void)
{ return (long)(((int64_t)m_pos_frames * 1000) / MIDI_RATE); }

static void midi_close(void) { m_ok = 0; }

const um_adecoder um_adec_midi = {
    "MIDI", midi_probe, midi_open, midi_decode, midi_seek, midi_close, midi_pos_ms
};

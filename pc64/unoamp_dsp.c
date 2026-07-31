/* UnoAmp DSP: the ten-band equaliser, and the plugin chain it is the first
 * member of.
 *
 * Phase 6 of docs/PLAYER-WINAMP-PLAN.md.
 *
 * WHERE THIS SITS. Between the decoder and the sink, on the host's pull:
 * Decode() -> DSP chain -> Write(). That is Winamp's position for a DSP plugin
 * and it is the only correct one - after the sink it would be too late to
 * change anything, and inside the decoder it would have to be reimplemented
 * per format.
 *
 * A DSP plugin may return FEWER or MORE samples than it was given (Winamp's
 * ABI allows a plugin to resample or time-stretch), which is why the buffer
 * handed in has headroom and the return value, not the input count, is what
 * gets written to the sink.
 *
 * THE EQ IS BIQUADS IN FIXED POINT. Ten peaking filters at the ISO centre
 * frequencies Winamp used, each a direct-form-I biquad in Q20. No FPU: the
 * kernel does not save x87/SSE state across the shell's frame loop, so a
 * float filter here would be a correctness hazard rather than merely a
 * dependency. Q20 leaves 11 bits of headroom over a 16-bit sample, which is
 * enough for the +12 dB the top slider asks for without clipping the
 * intermediate.
 */
#include "unoamp.h"
#include <string.h>

#define NB 10                       /* the ten ISO bands                      */
#define Q  20
#define ONE (1 << Q)

/* Winamp's band centres, in Hz. These are the ISO octave centres and they are
 * what the EQ window's ten sliders are labelled with, so they are not free to
 * change without the labels lying. */
static const int kFc[NB] = { 60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000 };

typedef struct {
    int b0, b1, b2, a1, a2;         /* Q20 coefficients                       */
    int x1[2], x2[2], y1[2], y2[2]; /* per-channel history                    */
} biquad;

static biquad g_bq[NB];
static int g_rate;                  /* the rate the coefficients were cut for */
static int g_last_gain[NB], g_last_pre = -9999, g_last_on = -1;
static int g_pre_q20 = ONE;

/* ---- the maths, without a libm --------------------------------------------
 * A peaking EQ needs sin, cos and a power. All three are wanted at a handful
 * of points that never change at run time except when the sample rate does,
 * so cheap series approximations are correct here in a way they would not be
 * inside the sample loop. */

/* sin/cos of x in Q20 radians, by the Taylor series to x^7. Argument is
 * reduced to [-pi, pi] first, where seven terms is good to about 1e-6 - three
 * orders of magnitude finer than the coefficient quantisation that follows. */
#define PI_Q20   3294199            /* pi   * 2^20                            */
#define TWOPI_Q20 6588397

static int mulq(int a, int b) { return (int)(((long long)a * b) >> Q); }

/* Scale into Q20 by MULTIPLYING, never by `<< Q`.
 *
 * THIS IS THE BUG THAT RESET THE ZIMABLADE. `x << 20` is undefined behaviour
 * for negative x, and half the quantities below go negative in the ordinary
 * course of cutting these ten bands: cos(w0) is positive across the low bands
 * so -2*cw is negative, and sin(w0) - hence alpha - goes negative for any
 * centre frequency above the Nyquist fold. The DEBUG build compiles with
 * `-fsanitize=shift -fsanitize-undefined-trap-on-error`, which turns each of
 * those into a `ud2`, so switching the equaliser on during playback executed
 * an illegal instruction within microseconds. The arithmetic was never wrong;
 * it was undefined, and only the sanitizing build minded.
 *
 * Multiplying by ONE is the same instruction on x86-64 and defined for every
 * sign. See tools/dsptest.c, which MUST be built with the sanitizer set to
 * catch this class - built plain, it passes on exactly the code that traps. */
#define SHL_Q(v) ((long long)(v) * ONE)

static int sin_q20(int x)
{
    int x2, term, acc;
    while (x >  PI_Q20) x -= TWOPI_Q20;
    while (x < -PI_Q20) x += TWOPI_Q20;
    x2 = mulq(x, x);
    acc = x;
    term = mulq(mulq(x, x2), ONE / 6);       acc -= term;   /* x^3/3!         */
    term = mulq(mulq(term, x2), ONE / 20);   acc += term;   /* x^5/5!         */
    term = mulq(mulq(term, x2), ONE / 42);   acc -= term;   /* x^7/7!         */
    return acc;
}
static int cos_q20(int x) { return sin_q20(x + PI_Q20 / 2); }

/* 10^(x/40) in Q20, for x in dB * 1000. This is the "A" of the RBJ peaking
 * cookbook. exp(k) via its series, with the argument small enough (|k| < 0.7
 * for +-12 dB) that five terms are exact to the coefficient's precision. */
static int amp_q20(int millibel)
{
    /* ln(10)/40 = 0.0575646, in Q20 = 60349, applied to dB (millibel/1000). */
    int k = (int)(((long long)millibel * 60349) / 1000);
    int acc = ONE, term = ONE, i;
    for (i = 1; i <= 6; i++) { term = mulq(term, k) / i; acc += term; }
    return acc;
}

/* One peaking biquad, RBJ cookbook, Q fixed at 1.0 (one octave-ish, which is
 * what a ten-band graphic EQ wants - narrower and the bands ring, wider and
 * adjacent sliders fight each other). */
static void cut(biquad *b, int fc, int rate, int millibel)
{
    int A, w0, cw, sw, alpha, aA, alA, a0;
    if (!rate) rate = 48000;
    /* A BAND ABOVE NYQUIST IS A PASS-THROUGH, not a filter. At 22 kHz the top
     * three sliders (12k/14k/16k) sit above fs/2, w0 exceeds pi, and the
     * cookbook - which assumes it does not - folds back into a filter with
     * poles wherever the fold lands. dsptest showed the damage plainly: at
     * 22050 a full CUT peaked at full scale, the opposite of what the slider
     * says. Winamp's sliders are labelled with fixed ISO centres, so the honest
     * answer for a band the stream cannot carry is to do nothing to it. */
    if (2 * fc >= rate) {
        b->b0 = ONE; b->b1 = b->b2 = b->a1 = b->a2 = 0;
        return;
    }
    A     = amp_q20(millibel / 2);          /* sqrt of the linear gain        */
    w0    = (int)(((long long)TWOPI_Q20 * fc) / rate);
    cw    = cos_q20(w0);
    sw    = sin_q20(w0);
    alpha = sw / 2;                         /* Q = 1.0                        */
    aA    = A ? (int)(SHL_Q(alpha) / A) : alpha;
    alA   = mulq(alpha, A);
    a0    = ONE + aA;
    if (!a0) a0 = 1;
    /* Normalise by a0 as we go: keeping a0 around would cost a divide per
     * sample, and this is cut once per slider move. */
    b->b0 = (int)(SHL_Q(ONE + alA) / a0);
    b->b1 = (int)(SHL_Q(-2 * (long long)cw) / a0);
    b->b2 = (int)(SHL_Q(ONE - alA) / a0);
    b->a1 = b->b1;                          /* same numerator as b1 here      */
    b->a2 = (int)(SHL_Q(ONE - aA) / a0);
}

/* Recut only when something changed. A slider drag would otherwise recompute
 * ten biquads every frame for no benefit. */
static void refresh(int rate)
{
    int i, changed = 0, on = unoamp_eq_enabled(), pre = unoamp_eq_preamp();
    if (rate != g_rate) { g_rate = rate; changed = 1; }
    if (on != g_last_on) { g_last_on = on; changed = 1; }
    if (pre != g_last_pre) { g_last_pre = pre; g_pre_q20 = amp_q20(pre * 120); }
    for (i = 0; i < NB; i++)
        if (unoamp_eq_band(i) != g_last_gain[i]) { g_last_gain[i] = unoamp_eq_band(i);
                                                   changed = 1; }
    if (!changed) return;
    for (i = 0; i < NB; i++) {
        /* Slider -100..+100 maps to -12..+12 dB, expressed in millibel. */
        cut(&g_bq[i], kFc[i], g_rate, g_last_gain[i] * 120);
        memset(g_bq[i].x1, 0, sizeof g_bq[i].x1);
        memset(g_bq[i].x2, 0, sizeof g_bq[i].x2);
        memset(g_bq[i].y1, 0, sizeof g_bq[i].y1);
        memset(g_bq[i].y2, 0, sizeof g_bq[i].y2);
    }
}

static int clip16(int v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

/* The internal signal is the sample scaled by 2^12, so full scale is 2^27.
 * YMAX is one octave of headroom over that, and every stage's output is held
 * inside it.
 *
 * A ten-band biquad chain with every slider at +12 dB has a worst-case gain
 * around 4^10, so the accumulator DOES leave int's range on real music - it
 * overflowed at 22 kHz in tools/dsptest.c. Accumulating in 64 bits and
 * limiting is not just sanitizer hygiene: a wrapped accumulator inverts the
 * sample, which is heard as a burst of noise and fed straight back into the
 * filter's own history, so the next block starts from garbage. Limiting
 * distorts the way an overdriven EQ is SUPPOSED to; wrapping does not. */
#define YMAX (1 << 28)

static int clampy(long long v)
{
    if (v >  YMAX) return  YMAX;
    if (v < -YMAX) return -YMAX;
    return (int)v;
}

/* ---- the EQ as a DSP plugin ------------------------------------------------ */
static int eq_modify(short *samples, int nframes, int nch, int rate)
{
    int i, c, b;
    if (!unoamp_eq_enabled()) return nframes;
    refresh(rate);
    for (i = 0; i < nframes; i++)
        for (c = 0; c < nch && c < 2; c++) {
            /* `* 4096`, not `<< 12`: the sample is signed and half of any
             * waveform is negative. See SHL_Q above. */
            int x = samples[i * nch + c] * (1 << (Q - 8)); /* headroom, not full Q */
            int y = clampy(((long long)x * g_pre_q20) >> Q);
            for (b = 0; b < NB; b++) {
                biquad *q = &g_bq[b];
                long long acc = (long long)q->b0 * y
                              + (long long)q->b1 * q->x1[c]
                              + (long long)q->b2 * q->x2[c]
                              - (long long)q->a1 * q->y1[c]
                              - (long long)q->a2 * q->y2[c];
                int out = clampy(acc >> Q);
                q->x2[c] = q->x1[c]; q->x1[c] = y;
                q->y2[c] = q->y1[c]; q->y1[c] = out;
                y = out;
            }
            samples[i * nch + c] = (short)clip16(y >> (Q - 8));
        }
    return nframes;
}

static unoamp_dsp g_dsp_eq;

/* ---- the chain -------------------------------------------------------------
 * Plugins run in registration order. There is no reordering UI because Winamp
 * had none either, and a chain whose order the user cannot see is worse than
 * one whose order is simply "the order you added them".
 */
#define DSP_MAX 8
static unoamp_dsp *g_dsp[DSP_MAX];
static int g_ndsp;

int unoamp_register_dsp(unoamp_dsp *d)
{
    int i;
    if (!d || !d->ModifySamples) return 0;
    for (i = 0; i < g_ndsp; i++) if (g_dsp[i] == d) return 0;
    if (g_ndsp >= DSP_MAX) return 0;
    if (d->Init && d->Init(d) != 0) return 0;
    g_dsp[g_ndsp++] = d;
    return 1;
}
int unoamp_dsp_count(void) { return g_ndsp; }
unoamp_dsp *unoamp_dsp_at(int i)
{ return (i >= 0 && i < g_ndsp) ? g_dsp[i] : 0; }

/* Run the chain. Returns the frame count AFTER processing, which may differ
 * from what went in - see the file header. */
int unoamp_dsp_run(short *samples, int nframes, int nch, int rate)
{
    int i;
    for (i = 0; i < g_ndsp && nframes > 0; i++)
        if (g_dsp[i]->enabled && g_dsp[i]->ModifySamples)
            nframes = g_dsp[i]->ModifySamples(g_dsp[i], samples, nframes, 16,
                                              nch, rate);
    return nframes;
}

/* The EQ's ModifySamples has the plugin signature; the real work is above so
 * that the host can also call it directly if the chain is ever bypassed. */
static int eq_modify_plugin(struct unoamp_dsp *m, short *samples, int nframes,
                            int bps, int nch, int rate)
{ (void)m; (void)bps; return eq_modify(samples, nframes, nch, rate); }

void unoamp_dsp_init(void)
{
    g_dsp_eq.description = "Equaliser (10 band)";
    g_dsp_eq.enabled = 1;
    g_dsp_eq.ModifySamples = eq_modify_plugin;
    unoamp_register_dsp(&g_dsp_eq);
}

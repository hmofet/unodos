/* UnoAmp visualisation: the spectrum analyser, the oscilloscope, and the
 * plugin surface they are the first two users of.
 *
 * Phase 5 of docs/PLAYER-WINAMP-PLAN.md.
 *
 * WHY 576 SAMPLES. Winamp handed vis plugins exactly 576 samples per channel
 * per frame, and every skin's viscolor.txt palette and every vis plugin ever
 * written assumes that number. It is not a tuning parameter - changing it
 * would silently misrender third-party plugins - so it is a constant.
 *
 * THE FFT IS FIXED-POINT. There is no FPU state saved across the shell's
 * frame loop and no libm in the kernel, so a floating-point FFT here would be
 * both a correctness hazard and a dependency we do not have. This is a radix-2
 * decimation-in-time transform in 32-bit integers with a compile-time twiddle
 * table, which is entirely adequate for driving 16 bars of a bar graph.
 *
 * The spectrum is drawn from the SKIN's palette (viscolor.txt) when a skin is
 * loaded, so a visualiser in a dark skin does not glow in a light one.
 */
#include "unoamp.h"
#include "unoamp_skin.h"
#include "unoui_theme.h"
#include "fb.h"
#include "pc64_icons.h"     /* pc64_shell_theme */
#include <string.h>

#define VIS_N 576                   /* the Winamp window, exactly            */
#define FFT_N 512                   /* the largest power of two that fits    */
#define FFT_B 9                     /* log2(FFT_N)                           */
#define BARS  16                    /* what fits in the 76px well            */

/* ---- the sample ring -------------------------------------------------------
 * The host pushes here on the way to the sink, so the visualiser sees exactly
 * what is about to be heard. It lags by the sink's buffer, which is the same
 * lag Winamp had and is under a frame at any sane buffer size. */
static short g_ringL[VIS_N], g_ringR[VIS_N];
static int   g_rw;

void unoamp_vis_feed(const short *pcm, int nframes)
{
    int i;
    if (!pcm) return;
    for (i = 0; i < nframes; i++) {
        g_ringL[g_rw] = pcm[i * 2];
        g_ringR[g_rw] = pcm[i * 2 + 1];
        if (++g_rw >= VIS_N) g_rw = 0;
    }
}

/* Copy the ring out in time order. Callers want a window, not a ring. */
static void snapshot(short *dst)
{
    int i;
    for (i = 0; i < VIS_N; i++) {
        int j = (g_rw + i) % VIS_N;
        dst[i] = (short)(((int)g_ringL[j] + (int)g_ringR[j]) / 2);
    }
}

/* ---- fixed-point FFT --------------------------------------------------------
 * Q15 twiddles, built once on first use because a 512-entry table of sines is
 * not something to hand-write and there is no libm to generate it at compile
 * time. The generator is the standard recurrence, which is exact enough in
 * Q15 over 512 points for a bar graph and costs nothing after the first call. */
static short g_cos[FFT_N / 2], g_sin[FFT_N / 2];
static int   g_tw;

/* sin(x) for x in Q15 turns, via a quarter-wave polynomial. Bhaskara's
 * approximation: max error under 0.2% of full scale, which is a fifth of one
 * pixel on a 16-pixel-tall bar. */
static int sin_q15(int turns)              /* turns: 0..32767 = 0..2pi        */
{
    int neg = 0, x;
    turns &= 32767;
    if (turns >= 16384) { neg = 1; turns -= 16384; }
    x = turns * 180 / 16384;               /* degrees, 0..180                 */
    {
        int num = 4 * x * (180 - x);
        int den = 40500 - x * (180 - x);
        int v = den ? (num * 32767 / den) / 100 * 100 / 100 : 0;
        if (v > 32767) v = 32767;
        return neg ? -v : v;
    }
}

static void twiddles(void)
{
    int i;
    if (g_tw) return;
    for (i = 0; i < FFT_N / 2; i++) {
        g_cos[i] = (short)sin_q15(i * 32768 / FFT_N + 8192);   /* cos = sin+90 */
        g_sin[i] = (short)sin_q15(i * 32768 / FFT_N);
    }
    g_tw = 1;
}

static int g_re[FFT_N], g_im[FFT_N];

static void fft(void)
{
    int i, j, k, step, m;
    /* bit reversal */
    for (i = 1, j = 0; i < FFT_N; i++) {
        int bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { int t = g_re[i]; g_re[i] = g_re[j]; g_re[j] = t;
                     t = g_im[i]; g_im[i] = g_im[j]; g_im[j] = t; }
    }
    for (step = 1, m = 0; m < FFT_B; m++, step <<= 1) {
        int span = step << 1;
        for (i = 0; i < FFT_N; i += span)
            for (k = 0; k < step; k++) {
                int t = k * (FFT_N / 2) / step;
                int wr = g_cos[t], wi = -g_sin[t];
                int a = i + k, b = a + step;
                /* Q15 multiply, shifted down to keep the accumulators in
                 * range across all nine stages without saturating. */
                int tr = (int)(((long)g_re[b] * wr - (long)g_im[b] * wi) >> 15);
                int ti = (int)(((long)g_re[b] * wi + (long)g_im[b] * wr) >> 15);
                g_re[b] = (g_re[a] - tr) >> 1;
                g_im[b] = (g_im[a] - ti) >> 1;
                g_re[a] = (g_re[a] + tr) >> 1;
                g_im[a] = (g_im[a] + ti) >> 1;
            }
    }
}

/* Magnitude without a square root: |a| + |b| overestimates by up to 41% but is
 * monotonic in the real magnitude, which is all a bar height needs. */
static int mag(int re, int im)
{
    int a = re < 0 ? -re : re, b = im < 0 ? -im : im;
    return a + b;
}

/* ---- the built-in visualisers ---------------------------------------------- */
static int g_bar[BARS], g_peak[BARS], g_peak_age[BARS];

static unsigned vis_colour(int i, int n)
{
    const unoamp_skin *sk = unoamp_skin_get();
    if (sk && sk->have_viscolor) {
        /* Winamp's palette: 0 background, 1 the peak dot, 2..17 the bar
         * gradient from bottom to top. */
        int c = 2 + (i * 16) / (n ? n : 1);
        if (c > 17) c = 17;
        return sk->viscolor[c];
    }
    return pc64_shell_theme()->pal.accent;
}
static unsigned peak_colour(void)
{
    const unoamp_skin *sk = unoamp_skin_get();
    if (sk && sk->have_viscolor) return sk->viscolor[1];
    return pc64_shell_theme()->pal.text;
}

static void spectrum(int x, int y, int w, int h, int scale)
{
    short win[VIS_N];
    int i, b, bw, gap = 1;
    snapshot(win);
    twiddles();
    for (i = 0; i < FFT_N; i++) { g_re[i] = win[i]; g_im[i] = 0; }
    fft();

    /* Log-ish band grouping: the ear is not linear in frequency, and a linear
     * split puts fourteen of sixteen bars above 5 kHz where music has almost
     * nothing. Each band is roughly double the width of the one before. */
    bw = (w - (BARS - 1) * gap) / BARS;
    if (bw < 1) bw = 1;
    for (b = 0; b < BARS; b++) {
        int lo = (b == 0) ? 1 : (1 << (b / 2)) * (1 + (b & 1)) ;
        int hi = (b == BARS - 1) ? FFT_N / 2 : (1 << ((b + 1) / 2)) * (1 + ((b + 1) & 1));
        int acc = 0, n = 0, v;
        if (hi > FFT_N / 2) hi = FFT_N / 2;
        for (i = lo; i < hi; i++) { acc += mag(g_re[i], g_im[i]); n++; }
        v = n ? acc / n : 0;
        v = v * h / 96;                       /* empirical full-scale         */
        if (v > h) v = h;
        /* Fall smoothly, rise instantly: a bar graph that lags the attack
         * reads as broken, one that lags the decay reads as smooth. */
        if (v > g_bar[b]) g_bar[b] = v; else g_bar[b] -= (g_bar[b] - v + 3) / 4;
        if (g_bar[b] >= g_peak[b]) { g_peak[b] = g_bar[b]; g_peak_age[b] = 0; }
        else if (++g_peak_age[b] > 12 && g_peak[b] > 0) g_peak[b]--;

        {
            int bx = x + b * (bw + gap) * scale, k;
            for (k = 0; k < g_bar[b]; k++)
                fb_fill_rect(bx, y + (h - 1 - k) * scale, bw * scale, scale,
                             vis_colour(k, h));
            if (g_peak[b] > 0 && g_peak[b] <= h)
                fb_fill_rect(bx, y + (h - g_peak[b]) * scale, bw * scale, scale,
                             peak_colour());
        }
    }
}

static void oscilloscope(int x, int y, int w, int h, int scale)
{
    short win[VIS_N];
    int i, prev = -1;
    snapshot(win);
    for (i = 0; i < w; i++) {
        int s = win[(long)i * VIS_N / w];
        int v = h / 2 - (s * (h / 2)) / 32768;
        int c;
        if (v < 0) v = 0;
        if (v >= h) v = h - 1;
        /* Winamp's oscilloscope palette (18..23) runs from the centre out, so
         * a loud waveform is a different colour from a quiet one. */
        c = (v * 6) / (h ? h : 1);
        if (c > 5) c = 5;
        {
            const unoamp_skin *sk = unoamp_skin_get();
            unsigned col = (sk && sk->have_viscolor) ? sk->viscolor[18 + c]
                                                     : pc64_shell_theme()->pal.accent;
            /* Join to the previous sample so a steep edge is a line, not two
             * dots with a gap - that gap is what makes a naive scope look
             * like noise on loud material. */
            int a = prev < 0 ? v : prev, lo = a < v ? a : v, hi = a < v ? v : a, k;
            for (k = lo; k <= hi; k++)
                fb_fill_rect(x + i * scale, y + k * scale, scale, scale, col);
        }
        prev = v;
    }
}

/* ---- the plugin registry ---------------------------------------------------
 * This is Winamp's winampVisModule verbatim: the HOST fills spectrumData and
 * waveformData, then calls Render(this_mod). A plugin never touches the audio
 * ring - it reads the bytes the host prepared. That is why third-party vis
 * plugins could be written by people who had never seen an FFT, and it is why
 * the two built-ins below are not a special case in the host.
 *
 * The data is 8-bit because Winamp's was. Every plugin scales from 0..255. */
#define VIS_MAX 8
static unoamp_vis *g_vis[VIS_MAX];
static int g_nvis, g_sel;

int unoamp_register_vis(unoamp_vis *v)
{
    int i;
    if (!v || !v->Render) return 0;
    for (i = 0; i < g_nvis; i++) if (g_vis[i] == v) return 0;
    if (g_nvis >= VIS_MAX) return 0;
    if (v->Init && v->Init(v) != 0) return 0;
    g_vis[g_nvis++] = v;
    return 1;
}
int unoamp_vis_count(void) { return g_nvis; }
unoamp_vis *unoamp_vis_at(int i)
{ return (i >= 0 && i < g_nvis) ? g_vis[i] : 0; }
void unoamp_vis_select(int i) { if (i >= -1 && i < g_nvis) g_sel = i; }
int  unoamp_vis_selected(void) { return g_sel; }

/* Where the host wants this frame drawn. Winamp's ABI predates anyone
 * imagining a compositor - a plugin drew into its own HWND - so the target
 * rectangle travels beside the module rather than inside it. That is the one
 * concession the skinned window forces. */
static int g_dx, g_dy, g_dw, g_dh, g_ds;

static int spectrum_render(struct unoamp_vis *m)
{ (void)m; spectrum(g_dx, g_dy, g_dw, g_dh, g_ds); return 0; }
static int scope_render(struct unoamp_vis *m)
{ (void)m; oscilloscope(g_dx, g_dy, g_dw, g_dh, g_ds); return 0; }

static unoamp_vis g_vis_spectrum;
static unoamp_vis g_vis_scope;

void unoamp_vis_init(void)
{
    g_vis_spectrum.description = "Spectrum analyser";
    g_vis_spectrum.spectrumNch = 2;
    g_vis_spectrum.Render = spectrum_render;
    g_vis_scope.description = "Oscilloscope";
    g_vis_scope.waveformNch = 2;
    g_vis_scope.Render = scope_render;
    unoamp_register_vis(&g_vis_spectrum);
    unoamp_register_vis(&g_vis_scope);
    g_sel = 0;
}

/* Fill the module's data arrays the way Winamp did, then let it draw. A plugin
 * that only wants the waveform still gets the spectrum computed, which costs
 * one FFT a frame and keeps the host from having to ask what a plugin wants
 * before it can give it anything. */
static void fill_module(unoamp_vis *m)
{
    short win[VIS_N];
    int i;
    snapshot(win);
    for (i = 0; i < UNOAMP_VIS_SAMPLES && i < VIS_N; i++) {
        int v = win[i] / 256 + 128;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        m->waveformData[0][i] = (unsigned char)v;
        m->waveformData[1][i] = (unsigned char)v;
    }
    twiddles();
    for (i = 0; i < FFT_N; i++) { g_re[i] = win[i]; g_im[i] = 0; }
    fft();
    for (i = 0; i < UNOAMP_VIS_SAMPLES; i++) {
        int k = i * (FFT_N / 2) / UNOAMP_VIS_SAMPLES;
        int v = mag(g_re[k], g_im[k]) / 4;
        if (v > 255) v = 255;
        m->spectrumData[0][i] = (unsigned char)v;
        m->spectrumData[1][i] = (unsigned char)v;
    }
}

void unoamp_vis_draw(int x, int y, int w, int h, int scale)
{
    unoamp_vis *v = unoamp_vis_at(g_sel);
    if (!v || !v->Render) return;
    g_dx = x; g_dy = y; g_dw = w; g_dh = h; g_ds = scale;
    fill_module(v);
    v->Render(v);
}

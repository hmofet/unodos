/* Host-side test for UnoAmp's equaliser.
 *
 * Enabling the EQ during playback took the ZimaBlade down twice. The DSP is
 * pure arithmetic over a buffer - no framebuffer, no disk, no hardware - so it
 * can be run natively, where an infinite loop shows up as a hang you can Ctrl-C
 * and a coefficient blow-up shows up as a number.
 *
 * BUILD IT WITH THE SANITIZERS THE DEBUG OS USES, OR IT LIES TO YOU. Built
 * plain, this harness passed every case on the exact code that was resetting
 * the box, and that clean bill of health cost a day: the defect was undefined
 * behaviour (`negative << 20`), which is correct arithmetic on x86 and a `ud2`
 * under `-fsanitize=shift -fsanitize-undefined-trap-on-error` - which is how
 * pc64's DEBUG build compiles. Match build.sh's set:
 *
 *   cc -O2 -I. -fsanitize=signed-integer-overflow,bounds,shift,\
 *      integer-divide-by-zero,null -o /tmp/dsptest tools/dsptest.c \
 *      unoamp_dsp.c -lm
 *   /tmp/dsptest
 *
 * Without -fsanitize-undefined-trap-on-error you get the file and line of each
 * violation instead of an instant SIGILL, which is what you want on the host.
 * A run that prints ANY "runtime error:" line is a failure even if every peak
 * and frame count is right.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unoamp.h"

/* The three accessors unoamp_dsp.c reads its state through, normally owned by
 * the EQ window. */
static int g_on = 1, g_pre = 0, g_band[10];
int unoamp_eq_enabled(void) { return g_on; }
int unoamp_eq_preamp(void)  { return g_pre; }
int unoamp_eq_band(int i)   { return (i >= 0 && i < 10) ? g_band[i] : 0; }

#define N 1152

static void run(const char *what, int rate, int nch, int pre, const int *bands)
{
    static short buf[N * 2];
    int i, got, peak = 0;
    g_pre = pre;
    for (i = 0; i < 10; i++) g_band[i] = bands ? bands[i] : 0;

    /* A full-scale-ish sine, the worst realistic input for a boosting EQ. */
    for (i = 0; i < N; i++) {
        int v = (int)(28000.0 * __builtin_sin(2.0 * 3.14159265 * 440.0 * i / rate));
        buf[i * nch] = (short)v;
        if (nch > 1) buf[i * nch + 1] = (short)v;
    }
    printf("%-28s rate=%-6d ch=%d ... ", what, rate, nch);
    fflush(stdout);
    got = unoamp_dsp_run(buf, N, nch, rate);
    for (i = 0; i < N * nch; i++) {
        int a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peak) peak = a;
    }
    printf("returned %d, peak %d%s\n", got, peak,
           got != N ? "   <-- FRAME COUNT CHANGED" : "");
}

int main(void)
{
    static const int flat[10]  = { 0 };
    static const int boost[10] = { 100, 100, 100, 100, 100, 100, 100, 100, 100, 100 };
    static const int cut_[10]  = { -100, -100, -100, -100, -100, -100, -100, -100, -100, -100 };
    static const int mix[10]   = { 100, -100, 60, -60, 0, 40, -40, 100, -100, 0 };
    int rates[] = { 44100, 48000, 22050, 11025, 8000, 96000 };
    unsigned r;

    unoamp_dsp_init();
    printf("dsp plugins: %d\n\n", unoamp_dsp_count());

    for (r = 0; r < sizeof rates / sizeof rates[0]; r++)
        run("flat", rates[r], 2, 0, flat);
    printf("\n");
    for (r = 0; r < sizeof rates / sizeof rates[0]; r++)
        run("all +12dB", rates[r], 2, 0, boost);
    printf("\n");
    for (r = 0; r < sizeof rates / sizeof rates[0]; r++)
        run("all -12dB", rates[r], 2, 0, cut_);
    printf("\n");
    run("mixed + preamp +12", 44100, 2, 100, mix);
    run("mixed, mono", 44100, 1, 0, mix);
    run("flat, preamp -12", 44100, 2, -100, flat);

    /* The transition that actually crashed the box: gains changing between
     * blocks, which is what dragging a slider during playback does. */
    printf("\nsweeping one band across its range while running:\n");
    {
        int g, b[10];
        memset(b, 0, sizeof b);
        for (g = -100; g <= 100; g += 5) {
            b[3] = g;
            run("  band3 sweep", 44100, 2, 0, b);
        }
    }
    printf("\nall runs completed - no hang\n");
    return 0;
}

/* unosound_test — host verification of the voice/score floor.
 *  - render a sustained A440 square note; verify pitch via zero-crossings.
 *  - render a short melody (one score) to build/unosound.wav as evidence.
 *  - confirm a `rest` (freq 0) is silent — the score drives the synth deterministically.
 */
#include "unosound.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define RATE 22050
static int fails = 0;
static void check(const char *w, int ok) { printf("  [%s] %s\n", ok ? "PASS" : "FAIL", w); if (!ok) fails++; }

int main(void) {
    static int16_t buf[RATE];                       /* up to 1s */
    /* 1. pitch: a 500ms A440 square wave */
    u_note_t a440[] = { {440, 500, U_SQUARE, 12} };
    size_t n = unosound_render(a440, 1, buf, RATE, RATE);
    int cross = 0;
    for (size_t i = 1; i < n; i++) if ((buf[i-1] < 0) != (buf[i] < 0)) cross++;
    double freq = cross / 2.0 / (n / (double)RATE);
    printf("    rendered %zu samples; measured ~%.0f Hz (want 440)\n", n, freq);
    check("synth pitch within 3% of requested 440 Hz", fabs(freq - 440.0) < 13.2);

    /* 2. a rest is silent */
    u_note_t rest[] = { {0, 100, U_SQUARE, 12} };
    size_t rn = unosound_render(rest, 1, buf, RATE, RATE);
    int silent = 1; for (size_t i = 0; i < rn; i++) if (buf[i] != 0) silent = 0;
    check("rest (freq 0) renders silence", silent && rn > 0);

    /* 3. a melody -> WAV (C major-ish run across waves) */
    static int16_t mel[RATE * 3];
    u_note_t tune[] = {
        {262,180,U_SQUARE,12},{294,180,U_SQUARE,12},{330,180,U_TRIANGLE,12},
        {349,180,U_TRIANGLE,12},{392,180,U_SAW,10},{440,180,U_SAW,10},
        {494,180,U_SQUARE,12},{523,360,U_TRIANGLE,13},{0,120,U_SQUARE,0},
    };
    size_t mn = unosound_render(tune, (int)(sizeof tune/sizeof tune[0]), mel, sizeof mel/2, RATE);
    int nonsilent = 0; for (size_t i = 0; i < mn; i++) if (mel[i] != 0) { nonsilent = 1; break; }
    check("melody renders non-silent PCM", nonsilent && mn > RATE);  /* > 1s */
    unosound_write_wav("build/unosound.wav", mel, mn, RATE);
    printf("    wrote build/unosound.wav (%zu samples, %.2fs)\n", mn, mn/(double)RATE);

    printf("\n%s\n", fails ? "FAILURES" : "ALL PASS");
    return fails ? 1 : 0;
}

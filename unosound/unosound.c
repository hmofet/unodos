/* unosound software floor: voice synth -> PCM, + WAV sink. See unosound.h. */
#include "unosound.h"
#include <stdio.h>
#include <string.h>

static uint32_t rng = 0x12345;
static int noise_bit(void) { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 1; }

/* one voice sample for phase t in [0,1) */
static int sample_wave(u_wave_t w, double phase) {
    switch (w) {
        case U_SQUARE:   return phase < 0.5 ? 1 : -1;
        case U_TRIANGLE: return (int)((phase < 0.5 ? (4*phase - 1) : (3 - 4*phase)) * 1);
        case U_SAW:      return (int)((2*phase - 1) * 1);
        case U_NOISE:    return noise_bit() ? 1 : -1;
    }
    return 0;
}

size_t unosound_render(const u_note_t *score, int n, int16_t *out, size_t cap, int rate) {
    size_t pos = 0;
    for (int k = 0; k < n; k++) {
        const u_note_t *nt = &score[k];
        size_t ns = (size_t)nt->dur_ms * rate / 1000;
        double amp = (nt->vol / 15.0) * 9000.0;       /* headroom below int16 max */
        if (nt->freq_hz == 0) {                        /* rest */
            for (size_t i = 0; i < ns && pos < cap; i++) out[pos++] = 0;
            continue;
        }
        double phase = 0.0, step = (double)nt->freq_hz / rate;
        for (size_t i = 0; i < ns && pos < cap; i++) {
            /* short attack/decay envelope to avoid clicks */
            double env = 1.0;
            if (i < ns/16) env = (double)i / (ns/16);
            else if (i > ns - ns/16) env = (double)(ns - i) / (ns/16);
            double v;
            if (nt->wave == U_TRIANGLE) v = (phase < 0.5 ? (4*phase - 1) : (3 - 4*phase));
            else if (nt->wave == U_SAW) v = (2*phase - 1);
            else v = sample_wave(nt->wave, phase);
            out[pos++] = (int16_t)(v * amp * env);
            phase += step; if (phase >= 1.0) phase -= 1.0;
        }
    }
    return pos;
}

int unosound_write_wav(const char *path, const int16_t *pcm, size_t nsamples, int rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    uint32_t data = (uint32_t)(nsamples * 2), riff = 36 + data, br = (uint32_t)rate * 2;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t sz16 = 16; uint16_t pcm1 = 1, ch1 = 1, ba = 2, bps = 16;
    uint32_t sr = (uint32_t)rate;
    fwrite(&sz16,4,1,f); fwrite(&pcm1,2,1,f); fwrite(&ch1,2,1,f); fwrite(&sr,4,1,f);
    fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 2, nsamples, f);
    fclose(f);
    return 0;
}

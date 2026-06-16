/* unosound — the audio subsystem (CONTRACT-ARCH §6, Phase 9).
 *
 * The floor sits at the VOICE/NOTE altitude, NOT at PCM (the §6 "audio-altitude
 * trap": SID/Paula/DOC/SPC700/PSG are synthesis chips, not PCM DACs). A score of
 * notes is the write-once input (the Tracker song format is byte-identical on every
 * port). Chiptune backends realize voices natively; PCM platforms software-synth the
 * same score into a mix buffer — this file is that software floor + a WAV sink for
 * host evidence.
 */
#ifndef UNOSOUND_H
#define UNOSOUND_H
#include <stdint.h>
#include <stddef.h>

typedef enum { U_SQUARE = 0, U_TRIANGLE, U_SAW, U_NOISE } u_wave_t;

typedef struct { uint16_t freq_hz; uint16_t dur_ms; u_wave_t wave; uint8_t vol; } u_note_t; /* vol 0..15 */

/* render `n` notes (sequential, one voice) into 16-bit mono PCM at `rate`.
 * Returns sample count written (caller sizes `out` generously). */
size_t unosound_render(const u_note_t *score, int n, int16_t *out, size_t cap, int rate);

int unosound_write_wav(const char *path, const int16_t *pcm, size_t nsamples, int rate);

#endif /* UNOSOUND_H */

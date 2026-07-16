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

/* ===========================================================================
 * Live sequencer (Phase 9 real-time floor). Where unosound_render() bakes a
 * score to PCM for host evidence, this plays a score LIVE on a single hardware
 * voice - the PC speaker (pc64), a PSG channel, the Sound Manager - through a
 * note_on(midi) / note_off() backend. Games loop a background song and fire
 * one-shot SFX; the host advances playback by calling uno_seq_tick() ~60x/sec.
 * MIDI-note based (0 = rest), so the same song table drives every port.
 * ======================================================================== */
typedef struct { unsigned char midi, dur; } u_seqnote_t;   /* dur in ~1/60 s ticks */

void uno_seq_backend(void (*note_on)(int midi), void (*note_off)(void));
void uno_seq_init(void);                                   /* reset + silence      */
void uno_seq_beep(int midi, int ticks);                    /* one-shot SFX         */
void uno_seq_play(const u_seqnote_t *song, int count);     /* loop a song          */
void uno_seq_stop(void);
int  uno_seq_playing(void);
void uno_seq_tick(void);                                   /* advance ~60 Hz       */

#endif /* UNOSOUND_H */

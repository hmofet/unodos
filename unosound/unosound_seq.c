/* ===========================================================================
 * UnoSound live sequencer (see unosound.h). A single-voice, freestanding
 * real-time player: loop a background song + fire one-shot SFX over a
 * note_on/note_off backend, advanced by uno_seq_tick() ~60 times a second.
 *
 * One physical voice, so an SFX beep borrows it from the song for its duration
 * and the song resumes on the next tick after the beep ends.
 * ======================================================================== */
#include "unosound.h"

static void (*g_on)(int midi);
static void (*g_off)(void);

static const u_seqnote_t *g_song;
static int g_song_n, g_song_i, g_song_timer, g_playing;
static int g_beep_timer;                 /* >0 while a one-shot SFX owns the voice */

void uno_seq_backend(void (*note_on)(int midi), void (*note_off)(void))
{ g_on = note_on; g_off = note_off; }

void uno_seq_init(void)
{
    g_song = 0; g_song_n = g_song_i = g_song_timer = 0;
    g_playing = 0; g_beep_timer = 0;
    if (g_off) g_off();
}

void uno_seq_beep(int midi, int ticks)
{
    if (ticks < 1) ticks = 1;
    g_beep_timer = ticks;
    if (g_on) g_on(midi);
}

void uno_seq_play(const u_seqnote_t *song, int count)
{
    if (!song || count <= 0) return;
    g_song = song; g_song_n = count;
    g_song_i = count - 1;        /* so the first tick advances to note 0 */
    g_song_timer = 0; g_playing = 1;
}

void uno_seq_stop(void)
{
    g_playing = 0; g_song = 0;
    if (g_beep_timer <= 0 && g_off) g_off();
}

int uno_seq_playing(void) { return g_playing; }

void uno_seq_tick(void)
{
    if (g_beep_timer > 0) {                       /* SFX owns the voice */
        if (--g_beep_timer == 0) {
            if (g_off) g_off();
            g_song_timer = 0;                     /* let the song retrigger next tick */
        }
        return;
    }
    if (!g_playing || !g_song) return;
    if (g_song_timer > 0) { g_song_timer--; return; }
    if (++g_song_i >= g_song_n) g_song_i = 0;     /* loop */
    g_song_timer = g_song[g_song_i].dur;
    if (g_song[g_song_i].midi) { if (g_on)  g_on(g_song[g_song_i].midi); }
    else                       { if (g_off) g_off(); }   /* rest */
}

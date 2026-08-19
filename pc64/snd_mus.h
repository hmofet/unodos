/* UnoDOS/pc64 - a game's own score: a Standard MIDI File played from MEMORY
 * (snd_mus.c).
 *
 * The Music app plays FILES: it opens a path through pc64_media's windowed
 * source and streams it. A game hands over bytes instead - Duum converts the
 * WAD's MUS lump to a Standard MIDI File in Python and passes the whole file
 * down - so this is the same unomedia MIDI decoder pointed at a buffer, with
 * the transport wired to the PCM stream.
 *
 * It takes the sample stream (UNO_SND_OWN_MUS) and gives it back when the
 * song stops. Effects do NOT go through here: they mix on top of whatever
 * holds the ring (uno_snd_sfx_* in snd_pcm.h), so a gunshot never interrupts
 * the score.
 *
 *   uno_snd_mus_play(smf, len, 1);      loop the level's music
 *   uno_snd_mus_tick();                 every shell frame (cheap when idle)
 *   uno_snd_mus_stop();
 */
#ifndef PC64_SND_MUS_H
#define PC64_SND_MUS_H

int  uno_snd_mus_play(const unsigned char *smf, long len, int loop);
void uno_snd_mus_stop(void);
int  uno_snd_mus_playing(void);
void uno_snd_mus_tick(void);       /* decode ahead; bounded by FIFO space */

#endif

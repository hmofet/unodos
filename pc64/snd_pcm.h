/* UnoDOS/pc64 - sampled audio: the PCM layer over HD Audio / AC'97
 * (snd_pcm.c).
 *
 * Modern PCs have no PC speaker; this gives the Sound Manager voice a real
 * DAC instead. uno_snd_init probes HDA first, then AC'97; on success a
 * 48 kHz s16 stereo DMA ring loops forever and uno_snd_poll keeps writing
 * the synthesised voice ahead of the hardware read position. When no PCM
 * device exists everything stays on the PIT-driven PC speaker.
 *
 * Two sources share that ring, one at a time:
 *   - the SQUARE VOICE (below): the Sound Manager / UnoSound backend, one
 *     note at a time, synthesised straight into the ring.
 *   - the SAMPLE STREAM: decoded audio pushed by the Music player. While a
 *     stream is open the square voice is muted and the ring is fed from the
 *     stream FIFO instead.
 *
 * ...and one source that is SUMMED ON TOP of whichever of those is playing:
 *   - the EFFECTS VOICES: short samples held by slot and triggered by a
 *     game. They mix into the ring as it is written, so firing a gun never
 *     has to take the ring away from the music.
 */
#ifndef PC64_SND_PCM_H
#define PC64_SND_PCM_H

void uno_snd_init(void);        /* probe + start (call once, pre-detach)     */
int  uno_snd_active(void);      /* 1 = a PCM device is streaming             */
const char *uno_snd_name(void); /* "HD Audio" / "AC'97" / "" when inactive   */
void uno_snd_note(int midi);    /* the square-wave voice (Sound Mgr backend) */
void uno_snd_quiet(void);
void uno_snd_volume(int pct);   /* 0..100 (the Control Panel slider)         */
void uno_snd_poll(void);        /* refill the DMA ring - call every frame    */

/* ---- the sample stream (decoded audio: WAV / MIDI / MP3 / AAC) -----------
 * The player pushes interleaved s16 frames at the DECODER's native rate and
 * channel count; this layer resamples to the ring's 48 kHz stereo as it
 * queues. Everything is push-driven from the app's tick, so a slow decode
 * underruns into silence rather than stalling the shell.
 *
 *   uno_snd_stream_begin(44100, 2);
 *   while ((n = uno_snd_stream_space()) > 0) {
 *       int got = decode(buf, n);
 *       if (!got) break;
 *       uno_snd_stream_write(buf, got);
 *   }
 */
void uno_snd_stream_begin(int rate, int channels); /* take the ring          */
void uno_snd_stream_end(void);                     /* give it back           */

/* Who holds the stream. Two producers pushing into one FIFO interleave two
 * decodes into mush, so a producer that did not open the stream must not
 * write to it: take it with the owning form below and re-check the owner on
 * every tick. Whoever calls begin LAST wins - that is the user opening the
 * Music app, and being displaced is not an error, it is a cue to stop.
 * uno_snd_stream_begin() means UNO_SND_OWN_MEDIA, so existing callers are
 * unchanged. */
#define UNO_SND_OWN_MEDIA 0            /* Music / UnoAmp: a decoded file      */
#define UNO_SND_OWN_MUS   1            /* snd_mus.c: a game's own score       */
void uno_snd_stream_begin_owned(int owner, int rate, int channels);
int  uno_snd_stream_owner(void);       /* meaningful while _open() is 1       */
int  uno_snd_stream_open(void);        /* 1 while a stream holds the ring    */
int  uno_snd_stream_space(void);       /* INPUT frames queueable right now   */
int  uno_snd_stream_write(const short *pcm, int nframes);  /* frames taken   */
void uno_snd_stream_pause(int paused);
int  uno_snd_stream_paused(void);
void uno_snd_stream_flush(void);       /* drop queued audio (seek / stop)    */
int  uno_snd_stream_queued(void);      /* OUTPUT frames still buffered       */
long uno_snd_stream_played(void);      /* output frames sent to the DAC      */
int  uno_snd_stream_level(void);       /* 0..100 peak since last call (VU)   */

/* ---- the effects voices --------------------------------------------------
 * A small sample bank plus a handful of voices, summed into the ring on top
 * of the square voice or the sample stream - so a gunshot does not silence
 * the score, which one-source-at-a-time would force it to.
 *
 * Samples are unsigned 8-bit mono at their own rate (what a Doom WAD's DS
 * lump carries), resampled per voice as they play. The slot index belongs to
 * the CALLER and means nothing here: a load replaces whatever was in it, and
 * the bank may DROP the least recently played slot when it is full, because
 * a caller that finds a slot silent can simply load it again.
 *
 *   uno_snd_sfx_load(3, pcm, n, 11025);   once, when the sound is first heard
 *   uno_snd_sfx_play(3, 255, 128);        vol 0..255, sep 0 L / 128 C / 255 R
 */
#define UNO_SFX_SLOTS 64

int  uno_snd_sfx_load(int slot, const unsigned char *pcm, int nsamples, int rate);
int  uno_snd_sfx_play(int slot, int vol, int sep);  /* 1 = a voice took it   */
void uno_snd_sfx_stop_all(void);       /* silence the voices, keep the bank  */
void uno_snd_sfx_free_all(void);       /* ...and drop the samples too        */
int  uno_snd_sfx_playing(void);        /* voices sounding right now          */

#endif

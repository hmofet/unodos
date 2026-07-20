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
int  uno_snd_stream_open(void);        /* 1 while a stream holds the ring    */
int  uno_snd_stream_space(void);       /* INPUT frames queueable right now   */
int  uno_snd_stream_write(const short *pcm, int nframes);  /* frames taken   */
void uno_snd_stream_pause(int paused);
int  uno_snd_stream_paused(void);
void uno_snd_stream_flush(void);       /* drop queued audio (seek / stop)    */
int  uno_snd_stream_queued(void);      /* OUTPUT frames still buffered       */
long uno_snd_stream_played(void);      /* output frames sent to the DAC      */
int  uno_snd_stream_level(void);       /* 0..100 peak since last call (VU)   */

#endif

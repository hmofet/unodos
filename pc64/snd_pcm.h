/* UnoDOS/pc64 - sampled audio: the PCM layer over HD Audio / AC'97
 * (snd_pcm.c).
 *
 * Modern PCs have no PC speaker; this gives the Sound Manager voice a real
 * DAC instead. uno_snd_init probes HDA first, then AC'97; on success a
 * 48 kHz s16 stereo DMA ring loops forever and uno_snd_poll keeps writing
 * the synthesised voice ahead of the hardware read position. When no PCM
 * device exists everything stays on the PIT-driven PC speaker. */
#ifndef PC64_SND_PCM_H
#define PC64_SND_PCM_H

void uno_snd_init(void);        /* probe + start (call once, pre-detach)     */
int  uno_snd_active(void);      /* 1 = a PCM device is streaming             */
const char *uno_snd_name(void); /* "HD Audio" / "AC'97" / "" when inactive   */
void uno_snd_note(int midi);    /* the square-wave voice (Sound Mgr backend) */
void uno_snd_quiet(void);
void uno_snd_volume(int pct);   /* 0..100 (the Control Panel slider)         */
void uno_snd_poll(void);        /* refill the DMA ring - call every frame    */

#endif

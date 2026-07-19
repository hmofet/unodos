/* UnoDOS/pc64 - Intel HD Audio (Azalia) driver (hdaudio.c).
 *
 * Brings up the first HDA controller + codec output path and loops a
 * 48 kHz / 16-bit / stereo DMA ring forever. The PCM layer (snd_pcm.c)
 * writes samples into the ring; position comes from the controller's DMA
 * position buffer (LPIB as the fallback when that never advances). */
#ifndef PC64_HDAUDIO_H
#define PC64_HDAUDIO_H

int       uno_hda_init(void);            /* 1 = streaming; 0 = no HDA / no path */
short    *uno_hda_ring(unsigned *frames);/* the loop buffer (interleaved L/R)   */
unsigned  uno_hda_pos(void);             /* hardware read position, in frames   */

#endif

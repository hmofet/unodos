/* UnoDOS/pc64 - AC'97 (Intel ICH-style) driver (ac97.c).
 *
 * Brings up the first AC'97 controller (PCI class 04/01) and loops a
 * 48 kHz / 16-bit / stereo DMA ring via the PCM-out bus master. The PCM
 * layer (snd_pcm.c) writes samples into the ring; uno_ac97_kick() keeps
 * LVI chasing CIV so the ring never halts. */
#ifndef PC64_AC97_H
#define PC64_AC97_H

int       uno_ac97_init(void);            /* 1 = streaming; 0 = no AC'97       */
short    *uno_ac97_ring(unsigned *frames);/* the loop buffer (interleaved L/R) */
unsigned  uno_ac97_pos(void);             /* hardware read position, in frames */
void      uno_ac97_kick(void);            /* advance LVI + self-heal a halt    */

#endif

/* cosmo64/afe.c -- the MT6771 AFE as pc64's PCM backend: the AUDIO power
 * domain, the codec over PWRAP, the DAC path, and a DL1 ring that snd_pcm.c
 * writes and the AFE reads forever.
 *
 * THE SHAPE. pc64/snd_pcm.c asks a backend for three things -- init(),
 * ring(&frames), pos() -- and does everything else itself: the square voice
 * the Sound Manager drives, the sample stream, the effects mixer, the
 * resampler, and the looping-ring design that makes underruns benign. So this
 * file is those three functions (uno_afe_*) and the bring-up under them; the
 * UNO_SND_BACKEND_AFE seam in snd_pcm.c picks them over HD Audio / AC'97.
 *
 * THE SEQUENCE, AND WHERE IT WAS PROVEN. afe_regs.h holds the register map
 * and the sequences; AFEPROBE.UNO (afeprobe.c) ran exactly those on the live
 * phone on 2026-09-05 with no reflash, and that is where the blocker of the
 * first two AUDIO=1 boots turned out to be: not the codec (23 of 23 rows
 * took), not the clock gates or muxes (all open at LK handover), but the
 * AUDIO MTCMOS in SPM, which the recovery slot's LK never turns on. With the
 * domain on, AFE_ON sticks, the DAC path's enable bits hold, and DL1 streams
 * at 192 KB/s = 48 kHz stereo s16. What the probe could NOT do is the codec:
 * PWRAP is deliberately reachable only through pmic.c's whitelist, so the
 * analog half is applied here, from the kernel, and this file is the first
 * place the whole chain runs together.
 *
 * ORDER, top to bottom, and why:
 *   1. the power domain -- everything below reads as zeros until it is on;
 *   2. the codec (pmic.c's measured 23-row set) -- the part that can hurt,
 *      done while the log is short and before anything is streaming into it;
 *   3. the DAC path, in the vendor's order (SetI2SDacOut, SetI2SDacEnable);
 *   4. the ring, silent, and DL1 on. From here snd_pcm.c owns the samples.
 *
 * THE RING IS DEVICE MEMORY. The AFE reads DRAM behind the CPU's back and is
 * not coherent with the caches, exactly like the xHCI (c64_usbglue.h). The
 * ring therefore lives in the ".xdma" section that flatten.py records and
 * mmu.c maps Device-nGnRnE, so a sample snd_pcm.c stores is in DRAM when the
 * store completes and no per-frame cache clean is needed. snd_pcm.c's "drain
 * before DMA reads" is a dsb on this architecture (its own seam).
 *
 * EVERY STEP LOGS AND FLUSHES BEFORE IT ACTS. If this wedges the machine, the
 * eMMC log names the last thing attempted. These are PMIC writes on a phone.
 */

#include "cosmo64.h"
#include "afe_regs.h"

int  c64_pmic_present(void);
int  c64_pmic_audio_apply(void);        /* pmic.c, behind C64_AUDIO */
void c64_log_flush(void);

/* 16384 frames = 64 KB = 341 ms at 48 kHz: comfortably past snd_pcm.c's
 * 200 ms lead, and a size the DL1 END register takes without complaint (Linux
 * runs a 48 KB ring through the same registers). */
#define RING_FRAMES 16384u
static short g_ring[RING_FRAMES * 2] __attribute__((section(".xdma"), aligned(64)));
static int   g_up;
/* The QEMU stand-in (below): no AFE reads the ring, so the read cursor is
 * computed from the generic timer instead -- 48 kHz from the moment DL1
 * "started". snd_pcm.c cannot tell the difference, which is the point. */
static int         g_sim;
static c64afe_u64  g_sim_t0;

static void afe_dump(const char *when)
{
    c64_logf("afe: %s dac0=%08x dac1=%08x i2s1=%08x conn3=%08x conn4=%08x "
             "uldl=%08x src2=%08x padtop=%08x dl1 %08x..%08x cur %08x\n",
             when, AFE_R32(AFE_DAC_CON0), AFE_R32(AFE_DAC_CON1),
             AFE_R32(AFE_I2S_CON1), AFE_R32(AFE_CONN3), AFE_R32(AFE_CONN4),
             AFE_R32(AFE_ADDA_UL_DL_CON0), AFE_R32(AFE_ADDA_DL_SRC2_CON0),
             AFE_R32(AFE_AUD_PAD_TOP_CFG), AFE_R32(AFE_DL1_BASE),
             AFE_R32(AFE_DL1_END), AFE_R32(AFE_DL1_CUR));
}

/* ---- the backend --------------------------------------------------------- */
int uno_afe_init(void)
{
    c64afe_u32 steps = 0, v;

    /* The QEMU gate boots this image too (urc.c makes the same test): the
     * virt board has no SPM at 0x10006000 and the first read would abort.
     * There is no sound to be had there -- but everything ABOVE this seam is
     * portable C the gate would otherwise never run: snd_pcm.c's voice, mixer
     * and resampler, the sequencer, and (since the Music slice) unomedia's
     * decoders behind the Music app, UnoAmp and the score player. So on the
     * virt board DL1 is stood in for: the same ring, and a read cursor that
     * the generic timer advances at exactly the hardware's 48 kHz. snd_pcm.c
     * writes ahead of a cursor that moves as the AFE's does, every consumer
     * runs to completion, and nothing is heard. The log says so in as many
     * words, because uno_snd_name() will still answer "MT6771 AFE". */
    if (c64_fdt_root_compat_has((const void *)FBDBG->dtb_ptr, "linux,dummy-virt")) {
        for (unsigned i = 0; i < RING_FRAMES * 2; i++)
            g_ring[i] = 0;
        g_sim_t0 = c64afe_cnt();
        g_sim = 1;
        g_up = 1;
        c64_log("afe: QEMU virt board -- no AFE; DL1 stands in as a RAM ring "
                "paced by the generic timer at 48 kHz (the PCM stack runs, "
                "nothing sounds)\n");
        return 1;
    }
    c64_log("afe: bringing the audio path up -- if this is the last line in "
            "the log, the SPM write took the machine down\n");
    c64_log_flush();

    /* 1. the power domain */
    if (!c64afe_domain_on(&steps)) {
        c64_logf("afe: the AUDIO domain never acked (sta=%08x con=%08x); "
                 "no audio this boot\n",
                 AFE_R32(SPM_PWR_STATUS), AFE_R32(SPM_AUDIO_PWR_CON));
        c64_log_flush();
        return 0;
    }
    c64_logf("afe: AUDIO domain %s%s%s (sta=%08x con=%08x); infra sta1=%08x "
             "topck cfg5=%08x\n",
             (steps & 2) ? "turned ON" : "already on",
             (steps & 1) ? ", project code written" : "",
             (steps & 4) ? ", an SRAM ack timed out" : "",
             AFE_R32(SPM_PWR_STATUS), AFE_R32(SPM_AUDIO_PWR_CON),
             AFE_R32(INFRA_PDN_STA1), AFE_R32(TOPCK_CLK_CFG_5));
    afe_dump("as found:");
    c64_log_flush();

    /* 2. the codec, over PWRAP */
    if (!c64_pmic_present()) {
        c64_log("afe: no PMIC -- the analog path cannot be brought up; the "
                "digital path is left off too\n");
        c64_log_flush();
        return 0;
    }
    c64_log("afe: applying the codec set (the vendor sequence, pmic.c)\n");
    c64_log_flush();
    if (c64_pmic_audio_apply() < 0) {
        c64_log("afe: the codec set did not apply cleanly -- see the readback "
                "lines above; not streaming into a half-built path\n");
        c64_log_flush();
        return 0;
    }

    /* 3. the DAC path */
    c64afe_dac_on();
    v = AFE_R32(AFE_DAC_CON0);
    if (!(v & 1u)) {
        c64_logf("afe: AFE_ON did not stick (dac0=%08x) with the domain on -- "
                 "a state the probe never saw; no audio this boot\n", v);
        c64_log_flush();
        return 0;
    }

    /* 3b. the speakers' own amplifiers, which are not the codec's: two
     *     external class-D parts behind GPIO153 / GPIO111 with a pulse-count
     *     gain mode, and the headphone-enable pin low (afe_regs.h). The first
     *     AUDIO=1 boot with a perfect codec set was silent for exactly this. */
    c64afe_extamp_on();
    c64_logf("afe: external speaker amps on (GPIO153=%d GPIO111=%d hp_en GPIO108=%d)\n",
             c64afe_gpio_get_out(GPIO_EXTAMP), c64afe_gpio_get_out(GPIO_EXTAMP2),
             c64afe_gpio_get_out(GPIO_HP_EN));

    /* 4. the ring, and DL1 reading it. Device memory: plain stores land. */
    for (unsigned i = 0; i < RING_FRAMES * 2; i++)
        g_ring[i] = 0;
    AFE_DSB();
    c64afe_dl1_start((c64afe_u32)(c64afe_u64)g_ring, RING_FRAMES * 4u);
    afe_dump("streaming:");
    g_up = 1;
    c64_log("afe: DL1 is streaming a 64 KB silent ring; snd_pcm.c owns it now\n");
    c64_log_flush();
    return 1;
}

short *uno_afe_ring(unsigned *frames)
{
    *frames = RING_FRAMES;
    return g_ring;
}

/* The DMA read cursor, in frames. Proven the right register on the device
 * (AUDIO-SURVEY.md: five reads 300 ms apart advance and wrap at END). */
unsigned uno_afe_pos(void)
{
    c64afe_u32 cur, base;
    if (g_sim)
        return (unsigned)(((c64afe_cnt() - g_sim_t0) * 48000ull / c64afe_freq())
                          % RING_FRAMES);
    cur  = c64afe_dl1_cur();
    base = (c64afe_u32)(c64afe_u64)g_ring;
    if (!g_up || cur < base) return 0;
    return ((cur - base) / 4u) % RING_FRAMES;
}

int uno_afe_active(void) { return g_up; }

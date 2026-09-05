/* cosmo64/afe.c -- first light for audio: the MT6771 AFE's own sine generator.
 *
 * WHY A SINE GENERATOR AND NOT A DMA RING. The AFE can emit a tone with no
 * buffer, no descriptors and no memory traffic at all -- one register. That
 * makes it the smallest thing that still proves everything underneath: that
 * the audio power domain is on at LK handover, that the block is clocked, that
 * the MT6358's analog chain can be brought up from bare metal, and that the
 * speaker is wired where we think it is. A DMA ring proves none of those and
 * adds its own failure modes on top. Get a tone, then add DL1.
 *
 * WHERE EVERY NUMBER HERE CAME FROM: cosmo64/AUDIO-SURVEY.md. Nothing in this
 * file is guessed from a datasheet -- the AFE offsets were confirmed against
 * two landmarks on the running device, and the codec values are what Linux
 * itself writes, captured by diffing the PMIC across an idle -> sine
 * transition TWICE and keeping only what changed identically both times. The
 * registers that moved on one run and not the other are noise (ADC readings,
 * counters) and are deliberately NOT written here; two of them, 0x248a and
 * 0x2492, were in an earlier single-run diff and would have been written if
 * that diff had been trusted.
 *
 * WHAT IS STILL UNKNOWN, and it is the ORDER. A diff gives a set of registers
 * and their target values; codec bring-up is order-sensitive and the order
 * could not be recovered (the survey says why: no /dev/mem, and regmap
 * tracing cannot see a PWRAP device). The order used below is the one that
 * makes physical sense -- supply and clock, then the codec's digital half,
 * then its analog half, then the AFE, then the tone last so nothing is
 * enabled into an unfinished path -- and it is a hypothesis, not a fact.
 *
 * EVERY STEP LOGS AND FLUSHES BEFORE IT ACTS. If this wedges the machine, the
 * eMMC log names the last thing attempted, which is the whole difference
 * between a bug and a mystery. That is not caution for its own sake: these are
 * PMIC writes on a phone.
 */

#include "cosmo64.h"

/* log.c keeps this as a local macro (so its host test can define it away);
 * there is no shared header for it, so this file carries its own. */
#ifndef C64_DSB
#define C64_DSB() __asm__ volatile("dsb sy" ::: "memory")
#endif

#define AFE_BASE       0x11220000ull
#define A32(off)       (*(volatile c64_u32 *)(AFE_BASE + (off)))

#define AFE_DAC_CON0   0x010u          /* bit 0 = AFE on, bit 1 = DL1 on   */
#define AFE_DAC_CON1   0x014u
#define AFE_DL1_BASE   0x040u
#define AFE_DL1_CUR    0x044u
#define AFE_DL1_END    0x048u
#define AFE_SGEN_CON0  0x1f0u

/* Measured on the device while Linux had the sine generator running. */
#define SGEN_ON        0x00580580u
#define DAC_CON1_VAL   0x00000aaau

int  c64_pmic_present(void);
int  c64_pmic_read(c64_u32 addr, c64_u32 *val);
int  c64_pmic_audio_apply(void);        /* pmic.c, behind C64_AUDIO */
void c64_log_flush(void);

static void afe_dump(const char *when)
{
    c64_logf("afe: %s DAC_CON0=%08x DAC_CON1=%08x SGEN=%08x "
             "DL1 base=%08x cur=%08x end=%08x\n",
             when, A32(AFE_DAC_CON0), A32(AFE_DAC_CON1), A32(AFE_SGEN_CON0),
             A32(AFE_DL1_BASE), A32(AFE_DL1_CUR), A32(AFE_DL1_END));
}

/* Is the block there at all? An unpowered or unclocked peripheral on this SoC
 * reads back all-ones (or takes the bus down, which is why this is the first
 * thing done and why the log is flushed before the first read). All-ones
 * across every register is the signature to stop on: it means the audio power
 * domain is off at handover and the next piece of work is SPM, not this file. */
static int afe_alive(void)
{
    c64_u32 a = A32(AFE_DAC_CON0), b = A32(AFE_DAC_CON1), c = A32(AFE_SGEN_CON0);
    if (a == 0xFFFFFFFFu && b == 0xFFFFFFFFu && c == 0xFFFFFFFFu)
        return 0;
    return 1;
}

void c64_afe_sine(void)
{
    c64_u32 v;

    c64_log("afe: probing the audio front-end at 0x11220000 -- if this is the "
            "last line in the log, the read itself took the machine down\n");
    c64_log_flush();

    if (!afe_alive()) {
        c64_log("afe: every register reads all-ones -- the audio power domain "
                "is OFF at handover. Nothing here can work until SPM turns it "
                "on; stopping before any write.\n");
        c64_log_flush();
        return;
    }
    afe_dump("as found:");
    c64_log_flush();

    /* 1. The codec, over PWRAP. This is the part that can hurt, so it goes
     *    first while the log is still short and everything after it is
     *    conditional on it having returned at all. */
    if (!c64_pmic_present()) {
        c64_log("afe: no PMIC -- the analog path cannot be brought up, so a "
                "tone would be inaudible even if the AFE ran. Stopping.\n");
        c64_log_flush();
        return;
    }
    c64_log("afe: applying the codec set (23 registers, AUDIO-SURVEY.md)\n");
    c64_log_flush();
    if (c64_pmic_audio_apply() < 0) {
        c64_log("afe: the codec set did not apply cleanly -- see the readback "
                "lines above; not enabling the generator into a half-built "
                "path\n");
        c64_log_flush();
        return;
    }

    /* 2. The AFE itself: rate config, then AFE_ON. DL1 is deliberately left
     *    off -- there is no ring, and the generator does not need one. */
    A32(AFE_DAC_CON1) = DAC_CON1_VAL;
    C64_DSB();
    v = A32(AFE_DAC_CON0);
    A32(AFE_DAC_CON0) = v | 1u;
    C64_DSB();
    v = A32(AFE_DAC_CON0);
    c64_logf("afe: AFE_ON -> DAC_CON0=%08x (wanted bit 0 set)\n", v);
    if (!(v & 1u)) {
        c64_log("afe: AFE_ON did not stick -- the block is mapped but not "
                "clocked. SPM/clock work comes before anything else here.\n");
        c64_log_flush();
        return;
    }

    /* 3. The tone, last. */
    A32(AFE_SGEN_CON0) = SGEN_ON;
    C64_DSB();
    v = A32(AFE_SGEN_CON0);
    c64_logf("afe: sine generator -> SGEN=%08x (wanted %08x)\n", v, SGEN_ON);
    afe_dump("after:   ");
    c64_log(v == SGEN_ON
            ? "afe: the generator is programmed. IF THE SPEAKER IS SILENT the "
              "digital side is running and the analog side is not -- which is "
              "the codec set's ORDER, the one thing the survey could not "
              "measure.\n"
            : "afe: the generator register did not take its value\n");
    c64_log_flush();
}

/* Turn it off again. The tone is continuous, and an OS that boots into an
 * unstoppable noise is not a good demonstration of anything. */
void c64_afe_sine_off(void)
{
    if (!afe_alive())
        return;
    A32(AFE_SGEN_CON0) = 0;
    C64_DSB();
    c64_log("afe: sine generator off\n");
}

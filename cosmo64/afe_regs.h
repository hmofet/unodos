/* cosmo64/afe_regs.h -- the MT6771 audio front-end, the SPM power domain
 * under it, and the register sequences that bring its DAC path up.
 *
 * ONE transcription, two users. afe.c (the kernel's audio backend) and
 * afeprobe.c (the AFEPROBE.UNO module that pokes the live phone with no
 * reflash) both include this, so the sequence the probe proved on hardware
 * is byte for byte the sequence the kernel runs. Everything is static inline,
 * takes no globals and logs nothing: callers read back and log.
 *
 * WHERE EACH NUMBER CAME FROM. The vendor kernel on quill,
 * /work/cosmo-kbuild/src (the tree Trixie runs):
 *   drivers/clk/mediatek/clk-mt6771-pg.c   spm_mtcmos_ctrl_audio(): the
 *                                          AUDIO MTCMOS and its on-order
 *   drivers/clk/mediatek/clk-mt6771.c      AUDIO_TOP_CON0 gate bits
 *   sound/soc/mediatek/mt6771/mtk-auddrv-afe.h        the offsets
 *   sound/soc/mediatek/mt6771/mt6771-sound.c          SetDLSrc2, SetSdmLevel,
 *                                          set_chip_adda_enable, the pad-top
 *                                          and MTKAIF resets
 *   sound/soc/mediatek/common_int/mtk-soc-afe-control.c
 *                                          SetI2SDacOut / SetI2SDacEnable:
 *                                          the ORDER the DAC path goes up
 *   sound/soc/mediatek/mt6771/mtk-soc-afe-connection.c
 *                                          DL1 (I05/I06) -> DAC (O03/O04) is
 *                                          AFE_CONN3 bit 5 + AFE_CONN4 bit 6
 * and cosmo64/AUDIO-SURVEY.md, measured on the device, which confirms the
 * interconnect (its "IRQ counter" rows 0x02c=0x20 / 0x030=0x40 ARE those two
 * bits), AFE_I2S_CON1=0xa0b, AFE_DAC_CON1=0xaaa and the DL1 ring registers.
 *
 * WHAT THE PROBE ESTABLISHED (2026-09-05, AFEPROBE.UNO on the phone): the
 * infracfg gate and the topckgen muxes are already open at LK handover; the
 * ONE thing missing was the AUDIO power domain. After c64afe_domain_on()
 * AFE_ON sticks, the generator register holds, and the DL1 cursor advances
 * at 192 KB/s -- 48 kHz stereo s16 exactly.
 */
#ifndef C64_AFE_REGS_H
#define C64_AFE_REGS_H

typedef unsigned int       c64afe_u32;
typedef unsigned long long c64afe_u64;

#define AFE_R32(a)  (*(volatile c64afe_u32 *)(c64afe_u64)(a))
#define AFE_DSB()   __asm__ volatile("dsb sy" ::: "memory")

/* ---- SPM: the AUDIO MTCMOS ----------------------------------------------- */
#define C64_SPM              0x10006000u
#define SPM_POWERON_CONFIG_EN (C64_SPM + 0x000)   /* project code 0x0b16, bit 0 */
#define SPM_PWR_STATUS       (C64_SPM + 0x180)    /* bit 24 = AUDIO             */
#define SPM_PWR_STATUS_2ND   (C64_SPM + 0x184)
#define SPM_AUDIO_PWR_CON    (C64_SPM + 0x314)
#define SPM_PWR_RST_B        (1u << 0)
#define SPM_PWR_ISO          (1u << 1)
#define SPM_PWR_ON           (1u << 2)
#define SPM_PWR_ON_2ND       (1u << 3)
#define SPM_PWR_CLK_DIS      (1u << 4)
#define SPM_AUDIO_STA        (1u << 24)

/* ---- INFRACFG / TOPCKGEN: read for the record, found open ---------------- */
#define C64_TOPCK            0x10000000u
#define TOPCK_CLK_CFG_5      (C64_TOPCK + 0x090)  /* audio_sel [1:0] pdn 7;
                                                   * aud_intbus_sel [9:8] pdn 15 */
#define C64_INFRA            0x10001000u
#define INFRA_PDN_STA1       (C64_INFRA + 0x094)  /* bit 25 = infra_audio       */
#define INFRA_PDN_STA2       (C64_INFRA + 0x0ac)  /* bit 4  = audio 26M bclk    */

/* ---- the AFE ------------------------------------------------------------- */
#define C64_AFE              0x11220000u
#define AUDIO_TOP_CON0       (C64_AFE + 0x000)    /* pdn: afe 2, dac 25, dac_predis 26 */
#define AUDIO_TOP_CON1       (C64_AFE + 0x004)
#define AFE_DAC_CON0         (C64_AFE + 0x010)    /* bit 0 AFE_ON, bit 1 DL1_ON */
#define AFE_DAC_CON1         (C64_AFE + 0x014)    /* [3:0] DL1 rate, bit 21 DL1 mono */
#define AFE_CONN3            (C64_AFE + 0x02c)    /* -> O03 (DAC L): bit 5 = I05 */
#define AFE_CONN4            (C64_AFE + 0x030)    /* -> O04 (DAC R): bit 6 = I06 */
#define AFE_I2S_CON1         (C64_AFE + 0x034)    /* the DAC out: SetI2SDacOut   */
#define AFE_DL1_BASE         (C64_AFE + 0x040)
#define AFE_DL1_CUR          (C64_AFE + 0x044)    /* the DMA read cursor         */
#define AFE_DL1_END          (C64_AFE + 0x048)
#define AFE_MEMIF_MSB        (C64_AFE + 0x0cc)
#define AFE_ADDA_DL_SRC2_CON0 (C64_AFE + 0x108)
#define AFE_ADDA_DL_SRC2_CON1 (C64_AFE + 0x10c)
#define AFE_ADDA_UL_DL_CON0  (C64_AFE + 0x124)    /* bit 0 = ADDA on             */
#define AFE_SGEN_CON0        (C64_AFE + 0x1f0)
#define AFE_ADDA_PREDIS_CON0 (C64_AFE + 0x260)
#define AFE_ADDA_PREDIS_CON1 (C64_AFE + 0x264)
#define AFE_MEMIF_HD_MODE    (C64_AFE + 0x3f8)    /* [1:0] DL1: 0 = 16-bit       */
#define AFE_MEMIF_HDALIGN    (C64_AFE + 0x3fc)
#define AFE_ADDA_DL_SDM_DCCOMP_CON (C64_AFE + 0xc50) /* [5:0] SDM level          */
#define AFE_ADDA_MTKAIF_CFG0 (C64_AFE + 0xe00)    /* 0 = protocol 1, no loopback */
#define AFE_AUD_PAD_TOP_CFG  (C64_AFE + 0xe40)    /* 0x31 = the pad-top FIFO on  */

/* the values, each named for where it came from */
#define AFE_DAC_CON1_48K_STEREO 0x00000aaau      /* measured; DL1 rate idx 0xa  */
#define AFE_I2S_CON1_DAC_48K    0x00000a0au      /* SetI2SDacOut(48000): rate<<8
                                                  * | I2S fmt<<3 | 32-bit WLEN<<1;
                                                  * bit 0 = enable, set last    */
#define AFE_DL_SRC2_48K         0x83001802u      /* SetDLSrc2(48000): idx 8<<28
                                                  * | mute off 3<<11 | x8 3<<24
                                                  * | 1<<1; bit 0 = enable      */
#define AFE_DL_SRC2_GAIN        0xf74f0000u      /* set_adda_dl_src_gain(unmute) */
#define AFE_SDM_LEVEL_NORMAL    0x1du            /* AUDIO_SDM_LEVEL_NORMAL       */
#define AFE_PAD_TOP_ON          0x31u
#define AFE_SGEN_MEASURED       0x00580580u      /* Linux's tone, amplitude 4    */

/* ---- time: the generic timer, so every wait is bounded ------------------- */
static inline c64afe_u64 c64afe_cnt(void)
{ c64afe_u64 v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static inline c64afe_u64 c64afe_freq(void)
{ c64afe_u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v ? v : 13000000ull; }
static inline void c64afe_spin_us(c64afe_u32 us)
{
    c64afe_u64 end = c64afe_cnt() + (c64afe_freq() * us) / 1000000ull;
    while ((long long)(end - c64afe_cnt()) > 0) ;
}
/* 1 iff (reg & mask) == want within `us` microseconds */
static inline int c64afe_wait(c64afe_u32 a, c64afe_u32 mask, c64afe_u32 want, c64afe_u32 us)
{
    c64afe_u64 end = c64afe_cnt() + (c64afe_freq() * us) / 1000000ull;
    for (;;) {
        if ((AFE_R32(a) & mask) == want) return 1;
        if ((long long)(end - c64afe_cnt()) <= 0) return (AFE_R32(a) & mask) == want;
    }
}

/* ---- the AUDIO power domain ---------------------------------------------- *
 * spm_mtcmos_ctrl_audio(STA_POWER_ON), with the unbounded status spins made
 * bounded. Returns 1 when the domain reports on (already or now), 0 when the
 * PWR_STATUS ack never came; *steps gets a bitmask of what was done:
 * 1 = wrote the project code, 2 = turned the domain on, 4 = an SRAM PDN ack
 * timed out (continued anyway: the vendor code notes the ack needs f26m_aud). */
static inline int c64afe_domain_on(c64afe_u32 *steps)
{
    c64afe_u32 v = AFE_R32(SPM_POWERON_CONFIG_EN);
    int i;
    *steps = 0;
    /* The register reads back as 1 after the write (the code field is not
     * readable), so the test is "bit 0 set", not "code present". */
    if (!(v & 1u)) {
        AFE_R32(SPM_POWERON_CONFIG_EN) = 0x0b160001u; AFE_DSB();
        *steps |= 1;
    }
    if ((AFE_R32(SPM_PWR_STATUS) & SPM_AUDIO_STA) &&
        (AFE_R32(SPM_PWR_STATUS_2ND) & SPM_AUDIO_STA))
        return 1;
    *steps |= 2;
    AFE_R32(SPM_AUDIO_PWR_CON) = AFE_R32(SPM_AUDIO_PWR_CON) | SPM_PWR_ON;     AFE_DSB();
    AFE_R32(SPM_AUDIO_PWR_CON) = AFE_R32(SPM_AUDIO_PWR_CON) | SPM_PWR_ON_2ND; AFE_DSB();
    if (!c64afe_wait(SPM_PWR_STATUS, SPM_AUDIO_STA, SPM_AUDIO_STA, 20000) ||
        !c64afe_wait(SPM_PWR_STATUS_2ND, SPM_AUDIO_STA, SPM_AUDIO_STA, 20000))
        return 0;
    AFE_R32(SPM_AUDIO_PWR_CON) = AFE_R32(SPM_AUDIO_PWR_CON) & ~SPM_PWR_CLK_DIS; AFE_DSB();
    AFE_R32(SPM_AUDIO_PWR_CON) = AFE_R32(SPM_AUDIO_PWR_CON) & ~SPM_PWR_ISO;     AFE_DSB();
    AFE_R32(SPM_AUDIO_PWR_CON) = AFE_R32(SPM_AUDIO_PWR_CON) | SPM_PWR_RST_B;    AFE_DSB();
    for (i = 0; i < 4; i++) {
        AFE_R32(SPM_AUDIO_PWR_CON) = AFE_R32(SPM_AUDIO_PWR_CON) & ~(1u << (8 + i)); AFE_DSB();
        if (!c64afe_wait(SPM_AUDIO_PWR_CON, 1u << (12 + i), 0, 20000))
            *steps |= 4;
    }
    return 1;
}

/* ---- the DAC path -------------------------------------------------------- *
 * SetI2SDacOut(48000) then SetI2SDacEnable(true), in the vendor's order:
 * configure everything with its enable bit clear, then AFE -> ADDA -> DL SRC
 * -> I2S out. The AUDIO_TOP_CON0 power-down bits for AFE/DAC/DAC_PREDIS read
 * clear at reset on this SoC; they are cleared again here because the vendor
 * clock driver does, and a cleared bit costs nothing. */
static inline void c64afe_dac_on(void)
{
    AFE_R32(AUDIO_TOP_CON0) = AFE_R32(AUDIO_TOP_CON0) & ~((1u << 2) | (1u << 25) | (1u << 26));
    AFE_DSB();
    AFE_R32(AFE_AUD_PAD_TOP_CFG)  = AFE_PAD_TOP_ON;        /* the link to the PMIC */
    AFE_R32(AFE_ADDA_MTKAIF_CFG0) = 0;                     /* protocol 1, no lpbk  */
    AFE_R32(AFE_ADDA_PREDIS_CON0) = 0;                     /* CleanPreDistortion   */
    AFE_R32(AFE_ADDA_PREDIS_CON1) = 0;
    AFE_R32(AFE_ADDA_DL_SRC2_CON0) = AFE_DL_SRC2_48K;      /* SetDLSrc2(48000)     */
    AFE_R32(AFE_ADDA_DL_SRC2_CON1) = AFE_DL_SRC2_GAIN;
    AFE_R32(AFE_ADDA_DL_SDM_DCCOMP_CON) =
        (AFE_R32(AFE_ADDA_DL_SDM_DCCOMP_CON) & ~0x3fu) | AFE_SDM_LEVEL_NORMAL;
    AFE_R32(AFE_I2S_CON1) = AFE_I2S_CON1_DAC_48K;
    AFE_R32(AFE_CONN3) = AFE_R32(AFE_CONN3) | (1u << 5);   /* DL1 L -> DAC L       */
    AFE_R32(AFE_CONN4) = AFE_R32(AFE_CONN4) | (1u << 6);   /* DL1 R -> DAC R       */
    AFE_DSB();
    /* SetI2SDacEnable(true) */
    AFE_R32(AFE_DAC_CON0) = AFE_R32(AFE_DAC_CON0) | 1u;             AFE_DSB();
    AFE_R32(AFE_ADDA_UL_DL_CON0) = AFE_R32(AFE_ADDA_UL_DL_CON0) | 1u; AFE_DSB();
    AFE_R32(AFE_ADDA_DL_SRC2_CON0) = AFE_R32(AFE_ADDA_DL_SRC2_CON0) | 1u; AFE_DSB();
    AFE_R32(AFE_I2S_CON1) = AFE_R32(AFE_I2S_CON1) | 1u;             AFE_DSB();
}

/* the reverse order; AFE_ON is left set, which is how Linux idles */
static inline void c64afe_dac_off(void)
{
    AFE_R32(AFE_ADDA_DL_SRC2_CON0) = AFE_R32(AFE_ADDA_DL_SRC2_CON0) & ~1u; AFE_DSB();
    AFE_R32(AFE_I2S_CON1) = AFE_R32(AFE_I2S_CON1) & ~1u;             AFE_DSB();
    AFE_R32(AFE_ADDA_UL_DL_CON0) = AFE_R32(AFE_ADDA_UL_DL_CON0) & ~1u; AFE_DSB();
    c64afe_spin_us(150);                        /* >= 1/fs before anything else */
}

/* ---- DL1: the memif that reads the ring ---------------------------------- *
 * 48 kHz, stereo, 16-bit interleaved, `bytes` long from a DRAM address that
 * the AFE will read behind the CPU's back: the caller keeps the ring out of
 * the data cache (a Device/NC mapping, or c64afe_clean() after each write). */
static inline void c64afe_dl1_start(c64afe_u32 base, c64afe_u32 bytes)
{
    AFE_R32(AFE_MEMIF_MSB) = AFE_R32(AFE_MEMIF_MSB) & ~(1u << 28);   /* CPU 8_24 fmt bit, as Linux */
    AFE_R32(AFE_MEMIF_HD_MODE) = AFE_R32(AFE_MEMIF_HD_MODE) & ~3u;   /* 16-bit                     */
    AFE_R32(AFE_MEMIF_HDALIGN) = AFE_R32(AFE_MEMIF_HDALIGN) & ~1u;
    AFE_R32(AFE_DAC_CON1) = AFE_DAC_CON1_48K_STEREO;
    AFE_R32(AFE_DL1_BASE) = base;
    AFE_R32(AFE_DL1_END)  = base + bytes - 1u;
    AFE_DSB();
    AFE_R32(AFE_DAC_CON0) = AFE_R32(AFE_DAC_CON0) | 2u;              /* DL1_ON */
    AFE_DSB();
}

static inline void c64afe_dl1_stop(void)
{
    AFE_R32(AFE_DAC_CON0) = AFE_R32(AFE_DAC_CON0) & ~2u;
    AFE_DSB();
}

static inline c64afe_u32 c64afe_dl1_cur(void) { return AFE_R32(AFE_DL1_CUR); }

/* clean a write-back-cached range to the point of coherency, so a DMA reader
 * sees it; line size from CTR_EL0 */
static inline void c64afe_clean(const void *p, unsigned long n)
{
    c64afe_u64 ctr, line, a, end;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line = 4ull << ((ctr >> 16) & 0xf);
    a = (c64afe_u64)p & ~(line - 1);
    end = (c64afe_u64)p + n;
    for (; a < end; a += line)
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    AFE_DSB();
}

/* ---- the external speaker amplifiers ------------------------------------- *
 * The Cosmo's speakers are NOT driven by the MT6358: they sit behind two
 * external class-D amplifiers, each switched by one GPIO with a pulse-count
 * gain mode (k71v1_64_bsp.dts: extamp = GPIO153, extamp2 = GPIO111;
 * mtk-auddrv-gpio.c AudDrv_GPIO_EXTAMP_Select: mode 3 = three low/high
 * pulses 2 us apart; mtk-soc-codec-6358.c Ext_Speaker_Amp_Change: headphone
 * enable (GPIO108) low first, both amps low, settle, pulse both, 25 ms warm
 * up). A PMIC register diff can never see this, which is why the first AUDIO=1
 * image with a perfect codec set stayed silent.
 *
 * GPIO block at 0x10005000: DIR at 0x000, DOUT at 0x100, MODE at 0x300 (eight
 * pins of four bits per word), 32 pins per DIR/DOUT word with a 0x10 stride;
 * every DIR/DOUT word has SET at +4 and CLR at +8. sdmmc.c uses the same map
 * for the card pins. */
#define C64_GPIO             0x10005000u
#define GPIO_EXTAMP          153
#define GPIO_EXTAMP2         111
#define GPIO_HP_EN           108

static inline void c64afe_gpio_out(unsigned pin)
{
    c64afe_u32 mode = C64_GPIO + 0x300u + (pin / 8u) * 0x10u;
    c64afe_u32 sh = (pin % 8u) * 4u;
    AFE_R32(mode) = AFE_R32(mode) & ~(0xfu << sh);            /* function 0 = GPIO */
    AFE_R32(C64_GPIO + 0x000u + (pin / 32u) * 0x10u + 4u) = 1u << (pin % 32u); /* DIR set */
    AFE_DSB();
}
static inline void c64afe_gpio_set(unsigned pin, int hi)
{
    AFE_R32(C64_GPIO + 0x100u + (pin / 32u) * 0x10u + (hi ? 4u : 8u)) = 1u << (pin % 32u);
    AFE_DSB();
}
static inline int c64afe_gpio_get_out(unsigned pin)
{
    return (AFE_R32(C64_GPIO + 0x100u + (pin / 32u) * 0x10u) >> (pin % 32u)) & 1u;
}

/* Ext_Speaker_Amp_Change(true), mode 3 on both amps. */
static inline void c64afe_extamp_on(void)
{
    int i;
    c64afe_gpio_out(GPIO_HP_EN);
    c64afe_gpio_out(GPIO_EXTAMP);
    c64afe_gpio_out(GPIO_EXTAMP2);
    c64afe_gpio_set(GPIO_HP_EN, 0);
    c64afe_gpio_set(GPIO_EXTAMP, 0);
    c64afe_gpio_set(GPIO_EXTAMP2, 0);
    c64afe_spin_us(2000);
    for (i = 0; i < 3; i++) {
        c64afe_gpio_set(GPIO_EXTAMP, 0);  c64afe_spin_us(2);
        c64afe_gpio_set(GPIO_EXTAMP, 1);  c64afe_spin_us(2);
    }
    for (i = 0; i < 3; i++) {
        c64afe_gpio_set(GPIO_EXTAMP2, 0); c64afe_spin_us(2);
        c64afe_gpio_set(GPIO_EXTAMP2, 1); c64afe_spin_us(2);
    }
    c64afe_spin_us(25000);
}

static inline void c64afe_extamp_off(void)
{
    c64afe_gpio_set(GPIO_EXTAMP, 0);
    c64afe_gpio_set(GPIO_EXTAMP2, 0);
    c64afe_spin_us(500);
}

#endif /* C64_AFE_REGS_H */

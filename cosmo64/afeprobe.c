/* cosmo64/afeprobe.c -- AFEPROBE.UNO: the audio path, poked from a running
 * UnoDOS with no reflash.
 *
 * WHY A MODULE AND NOT afe.c. The first two AUDIO=1 boots (AUDIO-SURVEY.md,
 * "what the hardware said") ended at AFE_ON refusing to stick: the block is
 * mapped and the codec applies, and the functional clock is not running. The
 * fix was somewhere in SPM (the AUDIO MTCMOS), INFRACFG (infra_audio),
 * topckgen (audio_sel / aud_intbus_sel) and the AFE's own AUDIO_TOP_CON0 --
 * four blocks, a dozen bits, and the only way to learn which of them matters
 * is to read them, flip them one at a time and watch AFE_ON. Doing that
 * through afe.c costs a reboot into Trixie, a flash, and a person at the LK
 * menu per attempt. A .UNO module costs a `put`, a `rescan` and a `launch`,
 * and the machine stays up: mmu.c identity-maps the whole first gigabyte as
 * Device memory, a module runs in the kernel's address space, and its log
 * lines land on the URC link as they are written. So this is a probe that
 * happens to be an app: open it and it runs the sequence once, showing every
 * readback in its window and on the SCRIPT log channel.
 *
 * WHAT THE FIRST RUN FOUND (2026-09-05): the AUDIO power domain was OFF
 * (PWR_STATUS bit 24 clear, AUDIO_PWR_CON=0xff12) and nothing else was
 * missing -- infra and topckgen read open. After the SPM on-sequence AFE_ON
 * stuck, the generator register held its value and the DL1 cursor advanced
 * at 192 KB/s, which is 48 kHz stereo s16 to the byte.
 *
 * WHAT THIS VERSION DOES: the whole digital path, as the kernel will run it
 * (afe_regs.h is shared with afe.c, so this IS the kernel's sequence): the
 * domain, the DAC path in the vendor's order, and DL1 reading a ring that
 * holds a real 441 Hz sine for three seconds. If the codec's analog half is
 * up -- the AUDIO=1 kernel applies it at boot -- the speaker plays the tone;
 * if it is not, the digital proof (cursor advancing through a configured
 * DAC path) is still the whole of what a module can show, because PWRAP is
 * not a module export and the codec is deliberately reachable only through
 * pmic.c's whitelist.
 *
 * SAFETY. Every write is to a register named by the vendor driver for this
 * purpose; every wait is bounded by the generic timer; nothing here is a
 * PMIC write. The tone stops itself after three seconds and when the window
 * closes. The clocks and AFE_ON are left as Linux idles them.
 */

#include "uno_uuiapp.h"
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "pc64_font.h"
#include "uno_appdesc.h"
#include "unoauto.h"
#include "unolog.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "afe_regs.h"

void pc64_shell_dirty(void);
const struct unoui_theme *pc64_shell_theme(void);
int  uno_snd_active(void);              /* snd_pcm.c, when AUDIO=1 built it in */
void uno_seq_beep(int midi, int ticks); /* the kernel's own voice, via the sequencer */

/* ---- output: a line buffer for the window, mirrored to the URC log -------- */
#define NLINES 64
#define LINEW  100
static char g_lines[NLINES][LINEW];
static int  g_n;

static void say(const char *fmt, ...)
{
    va_list ap;
    char *l = g_lines[g_n < NLINES ? g_n : NLINES - 1];
    va_start(ap, fmt);
    vsnprintf(l, LINEW, fmt, ap);
    va_end(ap);
    if (g_n < NLINES) g_n++;
    unoauto_log(UA_CH_SCRIPT, "afeprobe: %s", l);
    unolog(LOG_NOTICE, 0, "afeprobe: %s", l);
    pc64_shell_dirty();
}

/* ---- the ring: 48 KB of 48 kHz stereo s16 = 12288 frames = 256 ms -------- */
#define RING_BYTES  (48 * 1024)
#define RING_FRAMES (RING_BYTES / 4)
#define PERIODS     113                 /* whole periods per ring: no click at
                                         * the wrap. 113 * 48000 / 12288 = 441.4 Hz */
#define AMP         8000
static short g_ring[RING_FRAMES * 2] __attribute__((aligned(64)));

#define TONE_MS 3000
static int        g_state;              /* 0 idle, 1 playing, 2 done, 3 kernel chime */
static c64afe_u64 g_t0;
static c64afe_u32 g_cur0;
/* kernel mode: the chime through uno_seq_beep, one note per NOTE_MS, and the
 * loudest sample seen in the KERNEL's ring while it plays */
#define NOTE_MS 350
static const int  g_notes[] = { 60, 64, 67, 72 };
static int        g_note;
static c64afe_u64 g_note_t0;
static int        g_peak;
static volatile short *g_kring;
static unsigned   g_kframes;

static void fill_tone(void)
{
    int i;
    for (i = 0; i < RING_FRAMES; i++) {
        float ph = (float)(6.283185307f * (float)PERIODS * (float)i / (float)RING_FRAMES);
        short s = (short)(AMP * sinf(ph));
        g_ring[2 * i] = s;
        g_ring[2 * i + 1] = s;
    }
    c64afe_clean(g_ring, sizeof g_ring);
}

static void dump(const char *when)
{
    say("-- %s", when);
    say("spm: cfg=%08x sta=%08x sta2=%08x aud_pwr=%08x",
        AFE_R32(SPM_POWERON_CONFIG_EN), AFE_R32(SPM_PWR_STATUS),
        AFE_R32(SPM_PWR_STATUS_2ND), AFE_R32(SPM_AUDIO_PWR_CON));
    say("infra: sta1=%08x sta2=%08x  topck: cfg5=%08x",
        AFE_R32(INFRA_PDN_STA1), AFE_R32(INFRA_PDN_STA2), AFE_R32(TOPCK_CLK_CFG_5));
    say("afe: top0=%08x dac0=%08x dac1=%08x i2s1=%08x conn3=%08x conn4=%08x",
        AFE_R32(AUDIO_TOP_CON0), AFE_R32(AFE_DAC_CON0), AFE_R32(AFE_DAC_CON1),
        AFE_R32(AFE_I2S_CON1), AFE_R32(AFE_CONN3), AFE_R32(AFE_CONN4));
    say("adda: uldl=%08x src2=%08x/%08x sdm=%08x mtkaif=%08x padtop=%08x",
        AFE_R32(AFE_ADDA_UL_DL_CON0), AFE_R32(AFE_ADDA_DL_SRC2_CON0),
        AFE_R32(AFE_ADDA_DL_SRC2_CON1), AFE_R32(AFE_ADDA_DL_SDM_DCCOMP_CON),
        AFE_R32(AFE_ADDA_MTKAIF_CFG0), AFE_R32(AFE_AUD_PAD_TOP_CFG));
    say("dl1: base=%08x cur=%08x end=%08x hd=%08x msb=%08x sgen=%08x",
        AFE_R32(AFE_DL1_BASE), AFE_R32(AFE_DL1_CUR), AFE_R32(AFE_DL1_END),
        AFE_R32(AFE_MEMIF_HD_MODE), AFE_R32(AFE_MEMIF_MSB), AFE_R32(AFE_SGEN_CON0));
}

static void kernel_mode(void)
{
    c64afe_u32 base = AFE_R32(AFE_DL1_BASE), end = AFE_R32(AFE_DL1_END);
    say("KERNEL MODE: DL1 is already streaming the kernel's ring %08x..%08x and "
        "snd_pcm is active", base, end);
    say("sdm monitors before: fifo=%08x lch=%08x", AFE_R32(AFE_ADDA_DL_SDM_FIFO_MON),
        AFE_R32(AFE_ADDA_DL_SRC_LCH_MON));
    /* the second DAC connection Linux makes (O28/O29) and the memif normal mode */
    AFE_R32(AFE_CONN28) = AFE_R32(AFE_CONN28) | (1u << 5);
    AFE_R32(AFE_CONN29) = AFE_R32(AFE_CONN29) | (1u << 6);
    AFE_R32(AFE_MEMIF_HDALIGN) = AFE_R32(AFE_MEMIF_HDALIGN) | (0x7fffu << 16);
    AFE_DSB();
    say("conn28=%08x conn29=%08x hdalign=%08x (DL1 -> DAC_2 added)",
        AFE_R32(AFE_CONN28), AFE_R32(AFE_CONN29), AFE_R32(AFE_MEMIF_HDALIGN));
    say("extamp: before hp_en=%d amp=%d amp2=%d", c64afe_gpio_get_out(GPIO_HP_EN),
        c64afe_gpio_get_out(GPIO_EXTAMP), c64afe_gpio_get_out(GPIO_EXTAMP2));
    c64afe_extamp_on();
    say("extamp: ON (GPIO153 + GPIO111 pulsed x3, hp_en GPIO108 low): now hp_en=%d amp=%d amp2=%d",
        c64afe_gpio_get_out(GPIO_HP_EN), c64afe_gpio_get_out(GPIO_EXTAMP),
        c64afe_gpio_get_out(GPIO_EXTAMP2));
    g_kring = (volatile short *)(c64afe_u64)base;
    g_kframes = (end + 1u - base) / 4u;
    g_peak = 0;
    g_note = 0;
    g_note_t0 = c64afe_cnt();
    g_t0 = g_note_t0;
    g_cur0 = c64afe_dl1_cur();
    uno_seq_beep(g_notes[0], 60);
    g_state = 3;
    say("chime: C E G C through uno_seq_beep -> snd_pcm's square voice -> the kernel ring");
}

static void kernel_frame(void)
{
    c64afe_u64 now = c64afe_cnt();
    unsigned i, rd = (c64afe_dl1_cur() - (c64afe_u32)(c64afe_u64)g_kring) / 4u;
    /* peek at the 256 frames just behind the read cursor: what the DAC heard */
    for (i = 0; i < 256 && g_kframes; i++) {
        unsigned f = (rd + g_kframes - 1u - i) % g_kframes;
        int v = g_kring[f * 2];
        if (v < 0) v = -v;
        if (v > g_peak) g_peak = v;
    }
    if ((now - g_note_t0) * 1000ull / c64afe_freq() < NOTE_MS) return;
    g_note++;
    g_note_t0 = now;
    if (g_note < 4) { uno_seq_beep(g_notes[g_note], 60); return; }
    g_state = 2;
    say("chime: done after %llu ms; cur %08x -> %08x; peak |sample| = %d",
        (unsigned long long)((now - g_t0) * 1000ull / c64afe_freq()),
        g_cur0, c64afe_dl1_cur(), g_peak);
    say("chime: peak 0 means snd_pcm never wrote the ring; ~8400 is the square voice at volume 70");
    say("extamp: left ON so the shell's sounds can be heard");
    say("sdm monitors after: fifo=%08x lch=%08x", AFE_R32(AFE_ADDA_DL_SDM_FIFO_MON),
        AFE_R32(AFE_ADDA_DL_SRC_LCH_MON));
    dump("at the end");
}

static void start(void)
{
    c64afe_u32 steps = 0, v;
    dump("as found");
    if ((AFE_R32(AFE_DAC_CON0) & 3u) == 3u && uno_snd_active()) {
        kernel_mode();
        return;
    }
    if (!c64afe_domain_on(&steps)) {
        say("spm: the AUDIO domain never acked (sta=%08x con=%08x) -- stopping",
            AFE_R32(SPM_PWR_STATUS), AFE_R32(SPM_AUDIO_PWR_CON));
        g_state = 2;
        return;
    }
    say("spm: AUDIO domain %s%s%s (sta=%08x con=%08x)",
        (steps & 2) ? "turned ON" : "already on",
        (steps & 1) ? ", project code written" : "",
        (steps & 4) ? ", an SRAM ack timed out" : "",
        AFE_R32(SPM_PWR_STATUS), AFE_R32(SPM_AUDIO_PWR_CON));

    c64afe_dac_on();
    v = AFE_R32(AFE_DAC_CON0);
    say("dac path: dac0=%08x uldl=%08x src2=%08x i2s1=%08x conn3=%08x conn4=%08x -> %s",
        v, AFE_R32(AFE_ADDA_UL_DL_CON0), AFE_R32(AFE_ADDA_DL_SRC2_CON0),
        AFE_R32(AFE_I2S_CON1), AFE_R32(AFE_CONN3), AFE_R32(AFE_CONN4),
        (v & 1u) ? "AFE ON" : "AFE_ON DID NOT STICK");
    if (!(v & 1u)) { g_state = 2; return; }

    c64afe_extamp_on();
    say("extamp: ON (GPIO153 + GPIO111 pulsed x3, hp_en GPIO108 low)");
    fill_tone();
    c64afe_dl1_start((c64afe_u32)(c64afe_u64)g_ring, RING_BYTES);
    g_cur0 = c64afe_dl1_cur();
    g_t0 = c64afe_cnt();
    g_state = 1;
    say("dl1: PLAYING a %d Hz sine for %d ms: base=%08x end=%08x cur=%08x dac0=%08x",
        PERIODS * 48000 / RING_FRAMES, TONE_MS,
        AFE_R32(AFE_DL1_BASE), AFE_R32(AFE_DL1_END), g_cur0, AFE_R32(AFE_DAC_CON0));
}

static void stop(const char *why)
{
    c64afe_u32 c1 = c64afe_dl1_cur();
    c64afe_u64 el = (c64afe_cnt() - g_t0) * 1000ull / c64afe_freq();
    c64afe_dl1_stop();
    c64afe_dac_off();
    g_state = 2;
    say("dl1: stopped (%s) after %llu ms: cur %08x -> %08x, dac0=%08x",
        why, (unsigned long long)el, g_cur0, c1, AFE_R32(AFE_DAC_CON0));
    dump("at the end");
}

/* ---- the window ---------------------------------------------------------- */
static void draw(unoui_widget *w, unoui_rect c, void *ctx)
{
    const unoui_palette *p = &pc64_shell_theme()->pal;
    int i, y, h = uno_font_height_px(0, 12) + 2;
    (void)w; (void)ctx;
    fb_fill_rect(c.x, c.y, c.w, c.h, p->win_bg);
    y = c.y + 4;
    for (i = 0; i < g_n && y + h <= c.y + c.h; i++, y += h)
        fb_text(c.x + 6, y, g_lines[i], p->text, -1);
}

static unoui_canvas g_canvas = { draw, 0, 0 };

static void build(unoui_window *win)
{
    const unoui_metrics *m = &pc64_shell_theme()->m;
    int aw = fb_width() - 80, ah = fb_height() - 90;
    if (aw > 900) aw = 900;
    if (ah > 500) ah = 500;
    unoui_window_init(win, "AFE probe", 30, 24,
                      aw + 2 * m->frame_w + 2 * m->pad,
                      ah + m->title_h + 2 * m->pad + m->frame_w);
    unoui_add_canvas(win, 0, 0, aw, ah, &g_canvas);
}

static void opened(void)
{
    g_n = 0;
    g_state = 0;
    start();
}

static void frame(void)
{
    if (g_state == 3) { kernel_frame(); return; }
    if (g_state != 1) return;
    if ((c64afe_cnt() - g_t0) * 1000ull / c64afe_freq() >= TONE_MS)
        stop("timer");
}

static void closed(void)
{
    if (g_state == 1) stop("window closed");
}

static int canvas_index(void) { return 0; }

UNO_APP_DESC("id: afeprobe\n"
             "name: AFE probe\n"
             "short: AFE\n"
             "icon: sys\n"
             "cat: system\n"
             "rank: 99\n");

static const UnoUuiApp kApp = {
    UNO_UUIAPP_ABI, "AFE probe", build, 0, 0, frame, opened, closed, canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved) { (void)reserved; return &kApp; }

/* cosmo64/platform.c -- boot entry, time, and platform odds-and-ends for the
 * pc64 shell on the Cosmo. Implements the pc64_native.h time contract on the
 * ARM generic timer (no calibration dance: CNTFRQ_EL0 states the rate) and
 * the uno_pc64_* lifecycle/power/sound seam from mac_compat.h. */

#include "cosmo64.h"
#include "mac_compat.h"
#include "pc64_native.h"

int uno_main(void);

/* ---- boot: entry.s -> c_main -> the shell ------------------------------- */
void c_main(void *dtb)
{
    c64_beacon(224, 0xFFFF00FFu);   /* MAGENTA: C reached, the stack works   */
    /* Before the MMU, so that a translation fault has somewhere to say so:
     * with the MMU off every access is Device-nGnRnE and lands in DRAM
     * directly, which is exactly what the log wants anyway. */
    c64_log_survey();               /* before init() overwrites a signature */
    c64_log_init();
    c64_logf("dtb=%p cntfrq=%d\n", dtb, (int)c64_cnt_freq());
    c64_log_survey_report();
    mmu_init();
    c64_beacon(272, 0xFFFFFF00u);   /* YELLOW: translation + caches survived */
    c64_log("mmu on\n");
    c64_u32 ppitch;
    c64_u64 raw = c64_fb_adopt(dtb, &ppitch);  /* (its vram clear wipes the
                                                * beacons) */
    c64_logf("fb raw=%016x ppitch=%d src=%d vram=%x\n", raw, (int)ppitch,
             (int)FBDBG->fb_src, FBDBG->fb_vram);
    uno_native_tsc_set(c64_cnt_freq() / 1000000ull);
    /* How fast is this core actually running? LK hands over at whatever boot
     * frequency it chose and nothing here raises it -- Linux's cpufreq does
     * that, and bare metal has no cpufreq. The boot core is a little one
     * (this SoC is 4x A53 + 4x A73, 793 MHz at the bottom of both tables), so
     * a software renderer's ceiling may simply be low. Time a fixed loop
     * against the 13 MHz generic timer and put the number in the log rather
     * than reasoning about it. */
    {
        volatile int sink = 0;
        c64_u64 t0 = c64_cnt_now();
        for (int i = 0; i < 1000000; i++)
            sink = sink + i;
        c64_u64 us = (c64_cnt_now() - t0) * 1000000ull / c64_cnt_freq();
        c64_logf("cpu: 1e6 volatile add iterations in %d us (%d k-iter/s)\n",
                 (int)us, us ? (int)(1000000000ull / us) : 0);
    }
    /* Storage before the shell: session_load() runs inside uno_main. On QEMU
     * there is no MSDC at 0x11230000, so this costs one bounded command
     * timeout and logs that the eMMC is absent. */
    c64_blk_init();
    c64_log("entering uno_main\n");
    c64_log_flush();                /* the boot story reaches the eMMC even if
                                     * the shell never comes up */
    uno_main();                                   /* never returns */
    for (;;)
        __asm__ volatile("wfe");
}

/* ---- pc64_native.h: the time base --------------------------------------- */
static unsigned long long g_per_us;

unsigned long long uno_native_rdtsc(void)
{
    return c64_cnt_now();
}

void uno_native_tsc_set(unsigned long long cycles_per_us)
{
    g_per_us = cycles_per_us;
}

int uno_native_tsc_ok(void)
{
    return g_per_us != 0;
}

unsigned long long uno_native_tsc_per_us(void)
{
    return g_per_us;
}

void uno_native_delay_us(unsigned long us)
{
    c64_u64 until = c64_cnt_now() + (c64_u64)us * (g_per_us ? g_per_us : 13);
    while (c64_cnt_now() < until)
        __asm__ volatile("yield");
}

void uno_pc64_delay_ms(int ms)
{
    if (ms > 0)
        uno_native_delay_us((unsigned long)ms * 1000ul);
}

/* No RTC access yet (the MT6358 PMIC RTC is an M5 job). Per pc64_native.h:
 * rtc_read returns 1 on SUCCESS, 0 only for a dead/absent clock -- so 0. */
int uno_native_rtc_read(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    (void)y; (void)mo; (void)d; (void)h; (void)mi; (void)s;
    return 0;
}

int uno_native_rtc_write(int y, int mo, int d, int h, int mi, int s)
{
    (void)y; (void)mo; (void)d; (void)h; (void)mi; (void)s;
    return 0;
}

int uno_pc64_time(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    return uno_native_rtc_read(y, mo, d, h, mi, s);
}

int uno_pc64_set_time(int y, int mo, int d, int h, int mi, int s)
{
    return uno_native_rtc_write(y, mo, d, h, mi, s);
}

/* ---- lifecycle ----------------------------------------------------------- */
void uno_pc64_init(void)
{
    /* the heavy lifting (MMU, framebuffer, clock rate) already happened in
     * c_main before uno_main ran; nothing firmware-shaped left to do */
    c64_beacon(448, 0xFF00C0FFu);   /* LIGHT BLUE: the shell's init entered */
}

#ifdef C64_KBDTEST
/* KBDTEST=1 ./build.sh shell -- a scripted pad, like the asm port's AUTOTEST:
 * walk the launcher to the Editor, open it, and type. It drives the SAME ring
 * the AW9523 driver feeds, so the QEMU gate proves the whole typing path
 * (ring -> shell -> app -> glyphs on the panel) without hardware. */
void uno_pc64_inject_key(int scan, int uni, int ctrl);

static void kbdtest_tick(unsigned f)
{
    /* session_load() opens the Control Panel when there is no session file
     * (there is no storage yet), and it holds focus -- so close it first. */
    static const struct { unsigned at; int scan; int uni; int ctrl; } script[] = {
        { 40, 0, 'w', 1 },                    /* Ctrl-W:   close Control Panel */
        { 60, 0x17, 0, 1 },                   /* Ctrl-Esc: Start menu          */
        { 75, 0x02, 0, 0 },                   /* Down                          */
        { 90, 0, '\r', 0 },                   /* open it                       */
        { 130, 0, 'H', 0 }, { 140, 0, 'e', 0 }, { 150, 0, 'l', 0 },
        { 160, 0, 'l', 0 }, { 170, 0, 'o', 0 }, { 180, 0, ' ', 0 },
        { 190, 0, 'C', 0 }, { 200, 0, 'o', 0 }, { 210, 0, 's', 0 },
        { 220, 0, 'm', 0 }, { 230, 0, 'o', 0 }, { 240, 0, '!', 0 },
    };
    for (unsigned i = 0; i < sizeof script / sizeof script[0]; i++)
        if (script[i].at == f)
            uno_pc64_inject_key(script[i].scan, script[i].uni, script[i].ctrl);
}
#endif

void uno_pc64_poll(void)
{
    static int first = 1;
    static unsigned frames;
    if (first) {
        first = 0;
        c64_beacon(496, 0xFF008000u);   /* DARK GREEN: the main loop reached */
        c64_log("main loop\n");
        c64_kbd_init();
        c64_touch_init();
        c64_logf("input: keyboard %s, touch %s\n",
                 c64_kbd_present() ? "present" : "ABSENT",
                 c64_touch_present() ? "present" : "ABSENT");
    }
    /* Time the input drivers separately: they are polled I2C, and the AW9523
     * has no interrupt line, so a full matrix sweep is a real per-iteration
     * cost rather than a rounding error. */
    c64_u64 t0 = c64_cnt_now();
    c64_kbd_poll();
    c64_touch_poll();
    c64_perf_add_poll(c64_cnt_now() - t0);
    c64_perf_loop();
#ifdef C64_KBDTEST
    kbdtest_tick(frames);
#endif
    frames++;
    /* Push the log to the eMMC about twice a second. c64_log_flush() returns
     * immediately when nothing has been logged since the last one, so an idle
     * desktop costs nothing; a session that says something gets it on disk
     * before whatever happens next. */
    if ((frames % 30u) == 0u)
        c64_log_flush();
}

/* Reset via the TOPRGU watchdog: re-enable it and let it fire. LK armed it
 * with a ~30 s timeout; the SWRST path needs more register facts than we have
 * proven, so park if the trigger does not take. */
void uno_pc64_restart(void)
{
    *(volatile c64_u32 *)0x10007000ull = 0x22000001u;   /* key | EN */
    for (;;)
        __asm__ volatile("wfe");
}

void uno_pc64_shutdown(void)
{
    uno_pc64_restart();                          /* no PMIC power-off yet */
}

/* ---- sound: silent until the MTK AFE (M5, maybe never) ------------------ */
void uno_pc64_snd_note(int midi)
{
    (void)midi;
}

void uno_pc64_snd_quiet(void)
{
}

void uno_pc64_chime(void)
{
}

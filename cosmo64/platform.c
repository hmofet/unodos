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
    mmu_init();
    c64_beacon(272, 0xFFFFFF00u);   /* YELLOW: translation + caches survived */
    c64_u32 ppitch;
    c64_fb_adopt(dtb, &ppitch);     /* (its vram clear wipes the beacons)    */
    uno_native_tsc_set(c64_cnt_freq() / 1000000ull);
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

void uno_pc64_poll(void)
{
    static int first = 1;
    if (first) {
        first = 0;
        c64_beacon(496, 0xFF008000u);   /* DARK GREEN: the main loop reached */
        c64_kbd_init();
    }
    c64_kbd_poll();
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

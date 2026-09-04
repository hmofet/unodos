/* cosmo64/platform.c -- boot entry, time, and platform odds-and-ends for the
 * pc64 shell on the Cosmo. Implements the pc64_native.h time contract on the
 * ARM generic timer (no calibration dance: CNTFRQ_EL0 states the rate) and
 * the uno_pc64_* lifecycle/power/sound seam from mac_compat.h. */

#include "cosmo64.h"
#include "mac_compat.h"
#include "pc64_native.h"
#include "bootinfo.h"      /* uno_bios_find_ram: the loader's no-firmware seam */

int uno_main(void);
void uno_modload_reserve(void);    /* pc64_modload.c: carve the module arena */

/* ---- the module arena (M8) ----------------------------------------------
 * pc64_modload.c instantiates a .UNO into pages it gets from one of three
 * places: EFI AllocatePages while firmware is live, the E820 map on a BIOS
 * boot, or nothing. This payload has no firmware of either kind, and the
 * loader's "no system table" branch is the E820 one: uno_modload_reserve()
 * asks uno_bios_find_ram() for the arena once, at boot, and mod_alloc() bumps
 * through it from then on. So that seam is answered here with a static
 * carve-out instead of a map walk -- LK handed over all of DRAM already, and
 * mmu.c maps .bss as Normal write-back with no execute-never bit, which is
 * what code that will be jumped into needs. 4.5 MB: the loader's own
 * MOD_ARENA_PAGES + USER_SLOT_PAGES (a roster of ~40 KB modules twice over,
 * plus the fixed slot Studio's build-run loop reloads into), and the request
 * is checked against the size rather than trusted, so a loader that grows
 * its ask fails visibly here instead of overrunning whatever follows. */
#define MOD_ARENA_BYTES (1152u << 12)
static unsigned char g_mod_arena[MOD_ARENA_BYTES] __attribute__((aligned(4096)));

unsigned long long uno_bios_find_ram(const uno_bootinfo *bi,
                                     unsigned long long bytes)
{
    (void)bi;                       /* NULL here: there is no E820 block */
    if (bytes > MOD_ARENA_BYTES) {
        c64_logf("modload: arena asks %d KB, have %d KB -- no modules\n",
                 (int)(bytes >> 10), (int)(MOD_ARENA_BYTES >> 10));
        return 0;
    }
    c64_logf("modload: arena %d KB at %p\n", (int)(bytes >> 10), g_mod_arena);
    return (unsigned long long)(c64_u64)g_mod_arena;
}

/* ---- is this core actually cached? -------------------------------------- */
/* The first measurement said 1e6 volatile adds take 315,615 us: 315 ns for
 * what should be an L1 hit, which is a full bus transaction per access. That
 * is the signature of DEVICE-typed memory, and it agrees with the standing
 * rule that every file must build -mstrict-align or the machine wedges --
 * Device memory faults on unaligned access, Normal memory does not.
 *
 * Which raises the question M1 never actually tested: mmu_on() writes
 * SCTLR_EL1/TTBR0_EL1/TCR_EL1, and if LK enters this payload at EL2 those
 * writes are INERT -- translation there is governed by the EL2 registers --
 * and we have been running MMU-off, all memory Device, the whole time. An
 * identity map makes that indistinguishable by address: every pointer works
 * either way. M1 concluded "MMU on" from the vram band disappearing, and that
 * band was later re-explained as the shadow moving out of LK's page 1, so the
 * conclusion was never independently checked.
 *
 * So check it, and check the consequence directly: same number of accesses
 * over a 4 KB working set (L1-resident if there is an L1) and over a 1 MB one
 * (DRAM either way). Caches working means a large ratio; Device memory means
 * about 1. */
static volatile c64_u32 g_small[1024];
static volatile c64_u32 g_big[256 * 1024];

static void probe_cpu(void)
{
    c64_u64 el, s1 = 0, s2 = 0;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    el = (el >> 2) & 3;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(s1));
    c64_logf("cpu: CurrentEL=%d SCTLR_EL1=%08x (M=%d C=%d I=%d)\n",
             (int)el, s1, (int)(s1 & 1), (int)((s1 >> 2) & 1),
             (int)((s1 >> 12) & 1));
    if (el == 2) {
        __asm__ volatile("mrs %0, sctlr_el2" : "=r"(s2));
        c64_logf("cpu: *** RUNNING AT EL2 *** SCTLR_EL2=%08x (M=%d C=%d I=%d)"
                 " -- the EL1 registers mmu.c programs are INERT here\n",
                 s2, (int)(s2 & 1), (int)((s2 >> 2) & 1), (int)((s2 >> 12) & 1));
    }

    c64_u64 hz = c64_cnt_freq();
    c64_u64 t0 = c64_cnt_now();
    for (int i = 0; i < 200000; i++)
        g_small[i & 1023] = g_small[i & 1023] + 1;
    c64_u64 ts = c64_cnt_now() - t0;
    t0 = c64_cnt_now();
    for (int i = 0; i < 200000; i++)
        g_big[(i * 16) & (256 * 1024 - 1)] = g_big[(i * 16) & (256 * 1024 - 1)] + 1;
    c64_u64 tb = c64_cnt_now() - t0;
    c64_logf("cpu: 200k accesses -- 4KB set %d us, 1MB set %d us, ratio %d/10"
             " (about 10/10 means NO cache: every access is a bus cycle)\n",
             (int)(ts * 1000000ull / hz), (int)(tb * 1000000ull / hz),
             ts ? (int)(tb * 10ull / ts) : 0);

    volatile int sink = 0;
    t0 = c64_cnt_now();
    for (int i = 0; i < 1000000; i++)
        sink = sink + i;
    c64_u64 us = (c64_cnt_now() - t0) * 1000000ull / hz;
    c64_logf("cpu: 1e6 volatile add iterations in %d us (%d k-iter/s)\n",
             (int)us, us ? (int)(1000000000ull / us) : 0);
}

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
    mmu_init();          /* ...which finishes the .bss zero on its way out */
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
    probe_cpu();
    /* Storage before the shell: session_load() runs inside uno_main. On QEMU
     * there is no MSDC at 0x11230000, so this costs one bounded command
     * timeout and logs that the eMMC is absent. */
    c64_blk_init();
    /* The SD card is a real bring-up rather than an adoption, so it happens
     * here beside the eMMC and before anything asks for a volume; uno_blk_init
     * calls it again on its way to mounting and it is idempotent. On QEMU
     * there is no MSDC1 either, and it says so and moves on. */
    c64_sd_init();
    /* Mount before the shell rather than inside it: session_load() runs from
     * uno_main and asks for SHELL.CFG immediately, so a report printed after
     * that would only be a report of what the shell had already decided. */
    c64_storage_report();
    /* The module arena, before the shell: the first launch of a .UNO calls
     * mod_alloc(), and with nothing reserved every module "fails to load"
     * while the desktop draws fine -- the exact failure the loader's own
     * comment warns is easy to call working. */
    uno_modload_reserve();
    /* USB before the shell too: enumeration takes a moment (port power,
     * debounce, the hub walk) and the desktop should come up with its mouse
     * rather than acquire one a second later. */
    c64_usb_init();
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
        /* The rear cover panel, last of the three local input devices: its
         * bring-up walks three candidate pin pairs with a version query on
         * each, so it is the slow one, and it is the one the desktop can do
         * without. */
        c64_codi_init();
        /* The rear panel is deliberately NOT in this line. Its probe is a
         * state machine the poll drives, so at this instant it has only armed
         * its first candidate and would always report ABSENT -- which it did,
         * on the first hardware boot, three lines above the log saying the
         * touchpad was armed and reporting touch. codi.c announces itself when
         * it actually knows. */
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
    c64_codi_poll();                /* the rear panel's UART, drained and its
                                     * glide ticked, in the same budget */
    c64_usb_poll();                 /* polled xHCI too: a ring sweep per HID
                                     * endpoint, so it is timed with the rest */
    c64_perf_add_poll(c64_cnt_now() - t0);
    c64_perf_loop();
    c64_urc_tick();                 /* M6: bring the remote channel up once
                                     * the network bring-up has had its turn */
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

/* The URC `reboot` verb's reset (pc64_native.h: never returns). The TOPRGU
 * has an immediate software reset beside the timeout the restart above waits
 * out: MODE (+0x00, key 0x22000000) with EXTEN (bit 2, drive the external
 * reset line too) set and IRQ/DUAL modes clear, then SWRST (+0x14) with its
 * key 0x1209 -- register facts from the vendor mtk_wdt.h (wdt_v2), no code
 * copied. If the SoC has not gone within 100 ms, fall back to the watchdog. */
void uno_native_reset(void)
{
    volatile c64_u32 *mode = (volatile c64_u32 *)0x10007000ull;
    volatile c64_u32 *swrst = (volatile c64_u32 *)0x10007014ull;
    c64_log("reset: TOPRGU SWRST\n");
    c64_log_flush();
    c64_u32 m = *mode;
    m &= ~(0x8u | 0x40u);                        /* IRQ, DUAL modes off  */
    m |= 0x22000000u | 0x4u;                     /* key | EXTEN           */
    *mode = m;
    __asm__ volatile("dsb sy" ::: "memory");
    *swrst = 0x1209u;
    __asm__ volatile("dsb sy" ::: "memory");
    uno_native_delay_us(100000);
    uno_pc64_restart();                          /* never returns either  */
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

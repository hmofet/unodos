/* cosmo64/clock.c -- a wall clock for a machine that has no RTC.
 *
 * WHY THIS FILE EXISTS AT ALL: TLS. Everything else this port does is happy
 * with an uptime -- the taskbar prints "up 41s", file timestamps are nobody's
 * business, and no app has asked what year it is. Certificate validation is
 * different in kind: `br_x509_minimal_set_time()` decides whether a
 * certificate's validity window contains NOW, and with no clock pc64's
 * tls_now() falls back to its initialiser, 1970-01-01. Against that date every
 * certificate on the internet is NOT YET VALID, so CA-trusted HTTPS does not
 * fail obscurely -- it fails completely, on every host, forever.
 *
 * THE HARDWARE DOES HAVE A CLOCK AND THIS IS NOT IT. The MT6358 PMIC carries a
 * battery-backed RTC, and pmic.c already talks to that chip over PWRAP, so the
 * transport is solved -- what is missing is the register map, which is not in
 * this tree and which nothing here has ever read. Deriving it is a bring-up of
 * its own (the trick that would do it: read the RTC block from Trixie, where
 * the kernel's mt6397 driver knows the map, and compare). Until that happens
 * this file is what stands in, and it is deliberately shaped so that swapping
 * it for the PMIC costs two functions.
 *
 * WHAT IT IS: a monotonic software clock over CNTPCT_EL0, seeded from the best
 * of two answers and never allowed to go backwards.
 *
 *   1. The BUILD STAMP (C64_BUILD_EPOCH, from build.sh). An image knows when
 *      it was made, and that is a lower bound on "now" that costs nothing and
 *      is right within days for a fresh build.
 *   2. The LAST SAVED TIME (CLOCK.CFG on the preference volume -- the SD card,
 *      as of M7). Written when someone sets the clock, and re-written as it
 *      runs, so a reboot loses minutes rather than everything.
 *
 * The seed is max(1, 2), so a reboot cannot move the clock backwards and a
 * newer image cannot be dragged back by a stale file. Setting the time from
 * the shell's Date & Time pane goes through uno_native_rtc_write() and is
 * saved immediately, so a user's answer beats both.
 *
 * WHAT IT IS NOT: accurate. It drifts with CNTPCT, it does not know about time
 * zones (everything here is UTC, which is what BearSSL wants), and after a long
 * power-off it is behind by exactly that long. That is enough for certificate
 * validity, whose windows are measured in months, and it is not enough to be
 * anyone's watch. A certificate ISSUED after this image was built and never
 * corrected will read as not-yet-valid -- the one failure mode worth knowing
 * about, and the reason the boot log prints where the seed came from.
 */

#include "cosmo64.h"
#include "pc64_fs.h"

#define CLOCK_FILE   "CLOCK.CFG"
#define SAVE_EVERY   (15 * 60)          /* seconds of drift we accept losing */
#define EPOCH_SANE   1600000000ll       /* 2020-09-13: older than any build   */

static long long g_seed;                /* unix seconds at g_cnt0            */
static c64_u64   g_cnt0;
static long long g_saved_at;            /* clock value when we last wrote     */
static int       g_ready;
static int       g_from_file;

#ifndef C64_BUILD_EPOCH
#define C64_BUILD_EPOCH 0ll
#endif

/* ---- civil <-> days, Howard Hinnant's algorithms ------------------------- *
 * The same pair pc64's tls.c uses for the forward direction; both are exact
 * for every proleptic Gregorian date, which matters because a certificate
 * window straddling a leap day is not a hypothetical. */
static long days_from_civil(int y, int m, int d)
{
    long yy = y, era, yoe, doy, doe;
    yy -= (m <= 2);
    era = (yy >= 0 ? yy : yy - 399) / 400;
    yoe = yy - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, int *y, int *m, int *d)
{
    long era, doe, yoe, yy, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yy  = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    *d  = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m  = (int)(mp + (mp < 10 ? 3 : -9));
    *y  = (int)(yy + (*m <= 2));
}

/* ---- the file ------------------------------------------------------------ *
 * One decimal number and a newline. Plain text on purpose: this is a file a
 * person may have to read or delete from Trixie with the card mounted, and a
 * clock that cannot be corrected by hand is a clock that will need to be. */
static long long file_read(void)
{
    unsigned char buf[32];
    long n, i;
    long long v = 0;
    int vol = uno_fs_pref_vol();
    if (vol < 0) return 0;
    n = uno_fs_read(vol, CLOCK_FILE, buf, (long)sizeof buf - 1);
    if (n <= 0) return 0;
    for (i = 0; i < n; i++) {
        if (buf[i] < '0' || buf[i] > '9') break;
        v = v * 10 + (buf[i] - '0');
        if (v > 4000000000ll) return 0;             /* garbage, not a date    */
    }
    return v;
}

static void file_write(long long t)
{
    unsigned char buf[24];
    int i = 0, j;
    long long v = t;
    int vol = uno_fs_pref_vol();
    if (vol < 0 || !uno_fs_writable(vol) || t < EPOCH_SANE) return;
    while (v > 0 && i < 20) { buf[i++] = (unsigned char)('0' + (v % 10)); v /= 10; }
    for (j = 0; j < i / 2; j++) {
        unsigned char t2 = buf[j]; buf[j] = buf[i - 1 - j]; buf[i - 1 - j] = t2;
    }
    buf[i++] = '\n';
    if (uno_fs_write(vol, CLOCK_FILE, buf, (long)i) == 0)
        g_saved_at = t;
}

/* ---- the clock ----------------------------------------------------------- */
static long long clock_now(void)
{
    c64_u64 f = c64_cnt_freq();
    if (!g_ready) return 0;
    if (!f) f = 13000000ull;                        /* CNTFRQ is 13 MHz here  */
    return g_seed + (long long)((c64_cnt_now() - g_cnt0) / f);
}

/* Called from platform.c AFTER the storage report, so the preference volume
 * exists and CLOCK.CFG is reachable on the first read. Before this runs the
 * clock reports "absent" rather than 1970, which is the honest answer and the
 * one that keeps a pre-storage TLS attempt from validating against a lie. */
void c64_clock_init(void)
{
    long long build = (long long)C64_BUILD_EPOCH;
    long long saved = file_read();
    long long seed = build > saved ? build : saved;

    g_from_file = (saved > build);
    if (seed < EPOCH_SANE) {
        c64_log("clock: NO SEED -- no build stamp and no CLOCK.CFG; the clock "
                "reads as absent and CA-validated TLS will refuse\n");
        return;
    }
    g_seed = seed;
    g_cnt0 = c64_cnt_now();
    g_ready = 1;
    g_saved_at = saved;
    {
        int y, m, d;
        civil_from_days((long)(seed / 86400), &y, &m, &d);
        c64_logf("clock: seeded %04d-%02d-%02d %02d:%02d UTC from %s\n",
                 y, m, d, (int)((seed % 86400) / 3600), (int)((seed % 3600) / 60),
                 g_from_file ? "CLOCK.CFG" : "the build stamp");
    }
}

/* pc64_native.h: 1 on SUCCESS, 0 for a dead/absent clock. (The inverted-looking
 * convention is real -- see the note in platform.c and uno_native_rtc_read's
 * header; returning 0 here is what makes the taskbar print an uptime and what
 * makes tls.c's date fall back to the epoch.) */
int uno_native_rtc_read(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    long long t = clock_now();
    int yy, mm, dd;
    if (!g_ready || t < EPOCH_SANE) return 0;
    civil_from_days((long)(t / 86400), &yy, &mm, &dd);
    if (y)  *y  = yy;
    if (mo) *mo = mm;
    if (d)  *d  = dd;
    if (h)  *h  = (int)((t % 86400) / 3600);
    if (mi) *mi = (int)((t % 3600) / 60);
    if (s)  *s  = (int)(t % 60);
    /* Opportunistic save, from the READ path on purpose: the shell asks for
     * the time once a second to draw the taskbar, so this is a tick that
     * already exists and costs nothing to borrow. Without it a reboot would
     * throw away everything since the last time somebody SET the clock. */
    if (t - g_saved_at >= SAVE_EVERY) file_write(t);
    return 1;
}

int uno_native_rtc_write(int y, int mo, int d, int h, int mi, int s)
{
    long long t;
    if (y < 2020 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31)
        return 0;
    t = (long long)days_from_civil(y, mo, d) * 86400
        + (long long)h * 3600 + (long long)mi * 60 + s;
    if (t < EPOCH_SANE) return 0;
    g_seed = t;
    g_cnt0 = c64_cnt_now();
    g_ready = 1;
    file_write(t);
    c64_logf("clock: set to %04d-%02d-%02d %02d:%02d:%02d UTC\n", y, mo, d, h, mi, s);
    return 1;
}

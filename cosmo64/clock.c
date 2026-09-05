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
 * THE HARDWARE DOES HAVE A CLOCK, AND rtc.c NOW READS IT. The MT6358 carries a
 * battery-backed RTC; its register map was derived on the device and lives in
 * rtc.c. So this file has two modes, and which one it is in is printed at
 * boot:
 *
 *   HARDWARE. c64_rtc_read() answers, and the time is the PMIC's plus an
 *   offset (below). It survives power-off and it is the same clock Trixie and
 *   the TEE read. This is the normal case on the phone.
 *
 *   SOFTWARE. Nothing answers -- no PMIC at all under the QEMU gate, a dead
 *   coin cell, or an RTC whose fields are not a date -- and the fallback below
 *   takes over. This is what the machine did before rtc.c existed, kept
 *   because a gate with no PMIC still has to boot and still has to say
 *   something to the TLS stack.
 *
 * THE SOFTWARE FALLBACK: a monotonic clock over CNTPCT_EL0, seeded from the
 * best of two answers and never allowed to go backwards.
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
 * SETTING THE TIME WHEN THE HARDWARE CLOCK IS THE ONE TALKING. It is not
 * written back to the PMIC, because pmic.c has no way to write an arbitrary
 * register and this port is not going to grow one for a convenience (see
 * rtc.c). Instead CLOCK.CFG holds an OFFSET -- the line reads `off <signed
 * seconds>` rather than a bare epoch -- and the reported time is the RTC plus
 * that offset. On a phone whose RTC Linux keeps correct the offset is zero and
 * the file is never written at all.
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

int c64_rtc_read(int *y, int *mo, int *d, int *h, int *mi, int *s);

static long long g_seed;                /* unix seconds at g_cnt0            */
static c64_u64   g_cnt0;
static long long g_saved_at;            /* clock value when we last wrote     */
static int       g_ready;
static int       g_from_file;
static int       g_hw;                  /* the PMIC RTC is answering          */
static long long g_off;                 /* seconds to add to the PMIC RTC     */

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
static long long parse_num(const unsigned char *b, long n, int *neg)
{
    long i = 0;
    long long v = 0;
    *neg = 0;
    if (i < n && b[i] == '-') { *neg = 1; i++; }
    for (; i < n; i++) {
        if (b[i] < '0' || b[i] > '9') break;
        v = v * 10 + (b[i] - '0');
        if (v > 4000000000ll) return -1;            /* garbage, not a time    */
    }
    return v;
}

/* Two shapes, and the prefix says which: `off <n>` is an offset to add to the
 * hardware RTC, a bare number is an absolute time for the software clock. An
 * offset written on a phone whose RTC later dies is simply ignored, which is
 * the right way round -- a stale offset must never masquerade as a date. */
static long long file_read(int *is_off)
{
    unsigned char buf[32];
    long n;
    int neg = 0;
    long long v;
    int vol = uno_fs_pref_vol();
    *is_off = 0;
    if (vol < 0) return 0;
    n = uno_fs_read(vol, CLOCK_FILE, buf, (long)sizeof buf - 1);
    if (n <= 0) return 0;
    if (n > 4 && buf[0] == 'o' && buf[1] == 'f' && buf[2] == 'f' && buf[3] == ' ') {
        *is_off = 1;
        v = parse_num(buf + 4, n - 4, &neg);
        if (v < 0) return 0;
        return neg ? -v : v;
    }
    v = parse_num(buf, n, &neg);
    return (v < 0 || neg) ? 0 : v;
}

static void file_write_raw(long long v, int as_off)
{
    unsigned char buf[32];
    int i = 0, j, start;
    long long a = v < 0 ? -v : v;
    int vol = uno_fs_pref_vol();
    if (vol < 0 || !uno_fs_writable(vol)) return;
    if (as_off) { buf[i++] = 'o'; buf[i++] = 'f'; buf[i++] = 'f'; buf[i++] = ' '; }
    if (v < 0) buf[i++] = '-';
    start = i;
    if (a == 0) buf[i++] = '0';
    while (a > 0 && i < 28) { buf[i++] = (unsigned char)('0' + (a % 10)); a /= 10; }
    for (j = 0; j < (i - start) / 2; j++) {
        unsigned char t2 = buf[start + j];
        buf[start + j] = buf[i - 1 - j];
        buf[i - 1 - j] = t2;
    }
    buf[i++] = '\n';
    uno_fs_write(vol, CLOCK_FILE, buf, (long)i);
}

static void file_write(long long t)
{
    if (t < EPOCH_SANE) return;
    file_write_raw(t, 0);
    g_saved_at = t;
}

/* ---- the clock ----------------------------------------------------------- */
/* The hardware clock, as unix seconds, or 0. Cached for a second: the shell
 * asks for the time once a frame to draw the taskbar and there is no reason to
 * put seven PWRAP transactions on that path 36 times a second. */
static long long hw_now(void)
{
    static long long cache;
    static c64_u64 cache_at;
    c64_u64 f = c64_cnt_freq();
    int y, mo, d, h, mi, sec;

    if (!f) f = 13000000ull;
    if (cache && (c64_cnt_now() - cache_at) < f)
        return cache;
    if (!c64_rtc_read(&y, &mo, &d, &h, &mi, &sec))
        return 0;
    cache = (long long)days_from_civil(y, mo, d) * 86400
          + (long long)h * 3600 + (long long)mi * 60 + sec;
    cache_at = c64_cnt_now();
    return cache;
}

static long long clock_now(void)
{
    c64_u64 f = c64_cnt_freq();
    if (g_hw) {
        long long t = hw_now();
        if (t) return t + g_off;
        /* The RTC answered at boot and does not now. Do not silently fall
         * back to a seed that was taken from it minutes ago and call that a
         * clock -- say so once, and let the software clock take over. */
        g_hw = 0;
        c64_log("clock: the PMIC RTC stopped answering; falling back to the "
                "software clock\n");
        g_seed = g_seed ? g_seed : (long long)C64_BUILD_EPOCH;
        g_cnt0 = c64_cnt_now();
        g_ready = g_seed >= EPOCH_SANE;
    }
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
    int is_off = 0;
    long long saved = file_read(&is_off);
    long long hw = hw_now();
    long long seed;

    /* The hardware clock wins whenever it answers: it is the only one of the
     * three that knows how long the machine was switched off. */
    if (hw) {
        g_hw = 1;
        g_off = is_off ? saved : 0;
        {
            int y, m, d;
            long long t = hw + g_off;
            civil_from_days((long)(t / 86400), &y, &m, &d);
            c64_logf("clock: MT6358 RTC reads %04d-%02d-%02d %02d:%02d UTC%s\n",
                     y, m, d, (int)((t % 86400) / 3600), (int)((t % 3600) / 60),
                     g_off ? " (with a saved offset)" : "");
        }
        return;
    }

    if (is_off) saved = 0;              /* an offset with no RTC means nothing */
    seed = build > saved ? build : saved;
    g_from_file = (saved > build);
    if (seed < EPOCH_SANE) {
        c64_log("clock: NO SEED -- no PMIC RTC, no build stamp and no "
                "CLOCK.CFG; the clock reads as absent and CA-validated TLS "
                "will refuse\n");
        return;
    }
    g_seed = seed;
    g_cnt0 = c64_cnt_now();
    g_ready = 1;
    g_saved_at = saved;
    {
        int y, m, d;
        civil_from_days((long)(seed / 86400), &y, &m, &d);
        c64_logf("clock: no PMIC RTC; seeded %04d-%02d-%02d %02d:%02d UTC "
                 "from %s\n",
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
    if ((!g_ready && !g_hw) || t < EPOCH_SANE) return 0;
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
     * throw away everything since the last time somebody SET the clock.
     * Only for the SOFTWARE clock: a battery-backed RTC needs no help
     * remembering, and writing the card every quarter hour to tell it so
     * would be pure wear. */
    if (!g_hw && t - g_saved_at >= SAVE_EVERY) file_write(t);
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
    if (g_hw) {
        /* Correct the hardware clock without writing to it: keep the
         * difference and add it on every read. */
        long long hw = hw_now();
        if (hw) {
            g_off = t - hw;
            file_write_raw(g_off, 1);
            c64_logf("clock: set to %04d-%02d-%02d %02d:%02d:%02d UTC "
                     "(RTC offset %lld s, saved)\n", y, mo, d, h, mi, s, g_off);
            return 1;
        }
        g_hw = 0;                       /* it stopped answering mid-set */
    }
    g_seed = t;
    g_cnt0 = c64_cnt_now();
    g_ready = 1;
    file_write(t);
    c64_logf("clock: set to %04d-%02d-%02d %02d:%02d:%02d UTC\n", y, mo, d, h, mi, s);
    return 1;
}

/* cosmo64/rtc.c -- the MT6358's battery-backed real-time clock.
 *
 * clock.c has stood in for this since the HTTPS slice: a monotonic count over
 * CNTPCT seeded from the image's build stamp, which is enough for certificate
 * validity and is not a clock. This is the clock. It survives power-off, it is
 * the same one Trixie and the TEE read, and it means a certificate issued
 * after the image was built no longer reads as not-yet-valid.
 *
 * READ-ONLY, BY CONSTRUCTION. pmic.c fences writes behind a whitelist with no
 * address parameter, for the reason its header gives at length: every rail on
 * this board is behind those registers, and a wrong address is not a corrupted
 * partition but silicon at a voltage it was not built for. Nothing here needs
 * to write, so nothing here can: this file calls c64_pmic_read() and no other
 * PMIC entry point exists to it. Setting the time is handled one layer up, as
 * an OFFSET kept beside the clock file (clock.c), which costs nothing and
 * keeps the whitelist exactly as short as it was.
 *
 * HOW THE MAP WAS FOUND, because it is not in this tree and was not guessed.
 * Trixie runs on the same machine and MediaTek's kernel exposes the whole PMIC
 * register space read-only at /sys/kernel/debug/mtk_pmic/dump_pmic_reg. Two
 * dumps five seconds apart, diffed, leave a short list of registers that move
 * on their own -- and exactly one of them moved by five: 0x0592. From there
 * the neighbours read straight off against the wall clock (2026-09-05
 * 08:15:54): 0x0594 = 15, 0x0596 = 8, 0x0598 = 5, 0x059C = 9. Plain binary,
 * not BCD. Three independent cross-checks say the map is right rather than
 * coincidental:
 *
 *   1. 0x0592 tracked the wall second across a MINUTE ROLL -- 08:16:58 read
 *      sec=58 min=16, and 08:17:01 read sec=1 min=17.
 *   2. 0x0590 reads 0x10, and the kernel's own boot log prints
 *      "mtk_rtc_hal: 2nd RTC_AL_MASK = 0x10". That fixes the block's base at
 *      0x0588 with MediaTek's classic offsets (BBPU +0, IRQ_STA +2, IRQ_EN
 *      +4, CII_EN +6, AL_MASK +8, TC_SEC +0x0A...), which is what every other
 *      address here is derived from.
 *   3. 0x059E reads 58 and the year is 2026, so the epoch is 1968 -- which is
 *      MediaTek's documented RTC_MIN_YEAR. A single observation would not
 *      settle 1968 against 1970 or 2000; those give 2028 and 2058, and the
 *      device is not in either year.
 *
 * NO RELOAD IS NEEDED TO READ IT. MediaTek's own driver writes BBPU with a
 * key + RELOAD bit before reading, to latch the 32 kHz domain into the TC
 * registers. Measured here, the TC registers advance on their own between two
 * independent dumps with nothing writing BBPU in between, so a read sees live
 * values -- which is fortunate, because a reload is a WRITE and this file does
 * not have one. What can still happen is a TORN read, where the second rolls
 * between two of the seven reads and the fields disagree; that is handled the
 * way clocks have always handled it, by reading the seconds twice and retrying
 * when they differ.
 */

#include "cosmo64.h"

int c64_pmic_read(c64_u32 addr, c64_u32 *val);
int c64_pmic_present(void);

#define RTC_BASE      0x0588u
#define RTC_AL_MASK   (RTC_BASE + 0x08u)     /* 0x0590; reads 0x10, per dmesg */
#define RTC_TC_SEC    (RTC_BASE + 0x0Au)     /* 0x0592 */
#define RTC_TC_MIN    (RTC_BASE + 0x0Cu)
#define RTC_TC_HOU    (RTC_BASE + 0x0Eu)
#define RTC_TC_DOM    (RTC_BASE + 0x10u)
#define RTC_TC_DOW    (RTC_BASE + 0x12u)     /* not used: Y/M/D is enough     */
#define RTC_TC_MTH    (RTC_BASE + 0x14u)
#define RTC_TC_YEA    (RTC_BASE + 0x16u)     /* 0x059E; years since 1968      */

#define RTC_YEAR_BASE 1968

static int rd(c64_u32 addr, int *out)
{
    c64_u32 v = 0;
    if (c64_pmic_read(addr, &v) < 0)
        return -1;
    *out = (int)(v & 0xFFFFu);
    return 0;
}

/* One coherent sample, or a refusal. Seven reads cannot be atomic over a
 * serial bus, so read the seconds, read the fields, read the seconds again:
 * if the second did not move, nothing else did either and the sample is
 * consistent. Three attempts is generous -- a roll happens once per second
 * and the whole sequence takes tens of microseconds. */
int c64_rtc_read(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    int try;

    if (!c64_pmic_present())
        return 0;

    for (try = 0; try < 3; try++) {
        int sec0 = 0, sec1 = 0, mn = 0, hr = 0, dom = 0, mth = 0, yr = 0;

        if (rd(RTC_TC_SEC, &sec0) < 0 || rd(RTC_TC_MIN, &mn) < 0 ||
            rd(RTC_TC_HOU, &hr) < 0 || rd(RTC_TC_DOM, &dom) < 0 ||
            rd(RTC_TC_MTH, &mth) < 0 || rd(RTC_TC_YEA, &yr) < 0 ||
            rd(RTC_TC_SEC, &sec1) < 0)
            return 0;
        if (sec0 != sec1)
            continue;                    /* the second rolled mid-sample      */

        yr += RTC_YEAR_BASE;
        /* A dead coin cell, a PMIC that answered zeroes, or a map that turned
         * out to be wrong all present the same way: a date that is not a date.
         * Refuse it rather than hand the TLS stack a confident wrong answer --
         * clock.c then falls back to the build stamp, which is the behaviour
         * this machine had before this file existed. */
        if (yr < 2020 || yr > 2100 || mth < 1 || mth > 12 || dom < 1 ||
            dom > 31 || hr > 23 || mn > 59 || sec0 > 59)
            return 0;

        if (y)  *y  = yr;
        if (mo) *mo = mth;
        if (d)  *d  = dom;
        if (h)  *h  = hr;
        if (mi) *mi = mn;
        if (s)  *s  = sec0;
        return 1;
    }
    return 0;
}

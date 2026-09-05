/* cosmo64/test/logwin_test.c -- host unit test for the durable-log window.
 *
 * The eMMC log window (msdc.c) has to hold the boot story no matter how long
 * UnoDOS runs, even though the DRAM ring wraps and drops its oldest bytes. The
 * arithmetic that guarantees that -- freeze the first PRE bytes, and assemble
 * the window as [boot preamble][gap marker][recent tail] once the session
 * outruns the window -- lives in log.c (c64_log_window / c64_log_window_byte)
 * precisely so it can be tested here, off the device: the QEMU virt board has
 * no MSDC, so the gate (qharness.py) cannot exercise the eMMC path at all.
 *
 * This compiles the REAL log.c on the host by supplying the few things
 * cosmo64.h would (types, the zone, C64_EARLY) and defining the aarch64 full
 * barrier away -- it is irrelevant to the byte arithmetic under test.
 *
 * Build + run (on quill or any host with cc; there is no cc on amanuensis):
 *     cc -I. -o logwin_test test/logwin_test.c && ./logwin_test
 * It runs both scenarios itself and exits nonzero on any failure.
 */
#include <stdio.h>
#include <string.h>

/* ---- what cosmo64.h would give log.c, minimally ------------------------- */
typedef unsigned char      c64_u8;
typedef unsigned int       c64_u32;
typedef unsigned long long c64_u64;
#define C64_EARLY
#define C64_DSB()                        /* aarch64 barrier: nothing on host  */
static unsigned char g_test_zone[0x40000];
#define C64_LOG_ZONE ((unsigned long)(g_test_zone))
#define C64_LOG_SIZE 0x40000u
#define cosmo64_h_included
#define COSMO64_H                        /* keep the real header out if seen  */

void c64_log_flush(void) {}              /* c64_log_crash references it        */

/* pull in the code under test verbatim */
#include "../log.c"

/* ---- the fixture -------------------------------------------------------- */
#define PRE_BYTES 16384u
#define WINDOW    ((256u - 1u) * 512u)   /* eMMC window text cap = 130560      */

static unsigned char all[600000];        /* a mirror of every byte logged      */
static unsigned all_n;
static int fails;

static void put(const char *s)
{
    unsigned n = (unsigned)strlen(s);
    c64_log_write(s, n);
    memcpy(all + all_n, s, n);
    all_n += n;
}
#define CHECK(c, msg) do { \
    if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } \
    else       printf("  ok:   %s\n", msg); } while (0)

static int scenario(int long_run)
{
    /* fresh statics: each scenario runs in its own process (main re-execs via
     * argv), so g_pre_len / the ring start at zero here. */
    all_n = 0;
    c64_log_init();
    { const char *b = "\n=== UnoDOS cosmo64 ===\n";
      memcpy(all + all_n, b, strlen(b)); all_n += (unsigned)strlen(b); }

    for (int i = 0; i < 300; i++) {
        char l[64]; sprintf(l, "boot: line %03d of the opening story\n", i); put(l);
    }
    if (long_run)
        while (all_n < 420000) {
            char l[64]; sprintf(l, "perf: chatter %06u filling the ring\n", all_n); put(l);
        }

    unsigned gap = (unsigned)strlen(c64_log_gap);
    unsigned pre, g_, tailfrom, taillen;
    unsigned len = c64_log_window(WINDOW, &pre, &g_, &tailfrom, &taillen);
    printf("%s: total=%u ring=%u | window len=%u pre=%u gap=%u taillen=%u tailfrom=%u\n",
           long_run ? "LONG" : "SHORT", c64_log_total(), c64_log_bytes(),
           len, pre, g_, taillen, tailfrom);

    if (!long_run) {
        CHECK(pre == 0 && g_ == 0, "short: no preamble split");
        CHECK(len == c64_log_bytes() && tailfrom == 0, "short: whole ring verbatim");
        int ok = 1;
        for (unsigned i = 0; i < len && ok; i++)
            if (c64_log_window_byte(i, pre, g_, tailfrom) != all[i]) ok = 0;
        CHECK(ok, "short: window is the log verbatim, boot story at the head");
    } else {
        unsigned total = c64_log_total();
        CHECK(total > WINDOW, "long: session outran the window");
        CHECK(c64_log_bytes() == C64_LOG_SIZE - 12u, "long: ring is full (wrapped)");
        CHECK(c64_log_preamble_len() == PRE_BYTES, "long: preamble froze at PRE bytes");
        CHECK(pre == PRE_BYTES && g_ == gap && len == WINDOW,
              "long: window = [pre][gap][tail], fills the cap");
        CHECK(taillen == WINDOW - PRE_BYTES - gap, "long: tail fills the rest");
        int ok = 1;
        for (unsigned i = 0; i < len && ok; i++) {
            unsigned char exp;
            if (i < pre)            exp = all[i];                       /* boot */
            else if (i < pre + g_)  exp = (unsigned char)c64_log_gap[i - pre];
            else                    exp = all[total - taillen + (i - pre - g_)];
            if (c64_log_window_byte(i, pre, g_, tailfrom) != exp) {
                printf("    mismatch at %u\n", i); ok = 0;
            }
        }
        CHECK(ok, "long: boot preamble + marker + newest tail, byte-exact");
        CHECK(c64_log_window_byte(0, pre, g_, tailfrom) == '\n' &&
              c64_log_window_byte(1, pre, g_, tailfrom) == '=',
              "long: window opens on the boot banner");
        CHECK(c64_log_window_byte(len - 1, pre, g_, tailfrom) == all[total - 1],
              "long: window ends on the newest byte logged");
    }
    return fails;
}

int main(int argc, char **argv)
{
    /* one scenario per process so the file-scope statics start clean */
    if (argc > 1)
        return scenario(!strcmp(argv[1], "long"));

    char cmd[512];
    int rc = 0;
    printf("== short session ==\n");
    snprintf(cmd, sizeof cmd, "%s short", argv[0]); rc |= system(cmd);
    printf("== long session ==\n");
    snprintf(cmd, sizeof cmd, "%s long", argv[0]);  rc |= system(cmd);
    printf(rc ? "== FAILURES ==\n" : "== all pass ==\n");
    return rc ? 1 : 0;
}

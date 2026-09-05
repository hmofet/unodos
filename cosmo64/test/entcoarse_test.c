/* cosmo64/test/entcoarse_test.c -- is 13 MHz fine enough for the TLS entropy
 * health test?  (host test; runs on quill, not on the device)
 *
 * THE QUESTION. pc64's tls_entropy.c only credits its timing-jitter source if
 * the source passes a startup health test, and the part of that test this port
 * has to worry about is resolution: it wants the LOW FIVE BITS of the deltas to
 * take at least eight distinct values across 256 samples, over a workload that
 * lasts about a microsecond. On the Cosmo, CNTPCT_EL0 ticks at 13 MHz -- 77 ns
 * -- so a sample spans only tens of counts. On the QEMU virt board the same
 * register ticks at 62,500,000 Hz (16 ns, measured: the boot line reads
 * `cntfrq=62500000`), which is nearly five times finer. So the QEMU gate
 * reporting `entropy: source=jitter selftest=1` says nothing about the device,
 * and it would be easy to mistake it for an answer.
 *
 * WHAT THIS MEASURES, AND WHAT IT DOES NOT. It compiles the REAL tls_entropy.c
 * and feeds it the host's own monotonic clock QUANTISED to a chosen frequency,
 * so the jitter is genuine (a real memory system, real interrupts) and the only
 * variable is counter granularity. That isolates exactly the thing in doubt.
 *
 * It is evidence, not proof: an x86 build host's jitter distribution is not a
 * Cortex-A73's, and the honest test is the one the device prints on every boot.
 * What this can do is tell us BEFORE a flash whether 77 ns is obviously
 * hopeless -- and if 13 MHz fails here, it is very unlikely to pass there.
 *
 * Build and run on quill (there is no cc on amanuensis):
 *   cc -O1 -Wall -Wextra -DTLS_ENT_HOSTTEST -I../pc64 -I../pc64/bearssl/inc \
 *      -I../pc64/bearssl/src -o /tmp/entc cosmo64/test/entcoarse_test.c \
 *      pc64/tls_entropy.c pc64/bearssl/src/hash/sha2small.c \
 *      pc64/bearssl/src/codec/dec32be.c pc64/bearssl/src/codec/enc32be.c
 *   for f in 13000000 26000000 62500000 1000000000; do /tmp/entc $f; done
 *
 * One process per frequency: the probe's answer is cached in a file-static, so
 * scenarios must not share one (the same rule tls_entropy_test.sh follows).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tls_entropy.h"

static double g_hz = 13000000.0;

/* The host's real monotonic clock, quantised to g_hz. CLOCK_MONOTONIC is in
 * nanoseconds, so a tick at frequency f is ns * f / 1e9 -- the same information
 * loss a slower counter imposes, applied to real measurements. */
unsigned long long tls_ent_test_tsc(void)
{
    struct timespec ts;
    double ns;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ns = (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
    return (unsigned long long)(ns * g_hz / 1e9);
}

/* Force the jitter path: this port has no hardware DRNG (RNDR is ARMv8.5, the
 * MT6771 is ARMv8.0), so the question is only ever about the fallback. */
int tls_ent_test_has_rdrand(void) { return 0; }
int tls_ent_test_rdrand(unsigned long long *out) { (void)out; return 0; }

/* tls_entropy.c folds the link's frame counters in as diversity (never
 * credited). No net stack here. */
unsigned net_tx_frames(void) { return 0; }
unsigned net_rx_frames(void) { return 0; }

int main(int argc, char **argv)
{
    int src, self, i, ok;
    unsigned char a[32], b[32];
    const char *verdict;

    if (argc > 1) g_hz = atof(argv[1]);

    src  = tls_entropy_source();
    self = tls_entropy_selftest();

    /* Two draws, to say something about the output as well as the verdict.
     * tls_entropy_selftest() already checks this and throws its seeds away;
     * this is the same check with the bytes in hand, so a reader can see that
     * "healthy" is not a boolean somebody asserted. */
    ok = 0;
    if (src != TLS_ENT_NONE) {
        if (tls_entropy_get(a, sizeof a) && tls_entropy_get(b, sizeof b))
            ok = memcmp(a, b, sizeof a) != 0;
    }

    verdict = (src == TLS_ENT_NONE) ? "NO SOURCE -- TLS would refuse"
            : (self == 1 && ok)     ? "usable"
                                    : "CLAIMED A SOURCE BUT REPEATED ITSELF";

    printf("%12.0f Hz (%6.1f ns/tick): source=%-6s selftest=%2d draws-differ=%d  %s\n",
           g_hz, 1e9 / g_hz, tls_entropy_name(), self, ok, verdict);

    if (src != TLS_ENT_NONE) {
        printf("                              first 8 bytes:");
        for (i = 0; i < 8; i++) printf(" %02x", a[i]);
        printf("\n");
    }
    /* Exit non-zero only for the contradiction (a source that claims to be
     * live and is not). "No source" is a legitimate answer here -- it is the
     * finding, not a failure of the test. */
    return (self == -1) ? 1 : 0;
}

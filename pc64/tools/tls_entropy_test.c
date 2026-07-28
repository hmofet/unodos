/* Host gate for pc64/tls_entropy.c - the RNG behind TLS.
 *
 * The security argument for the fail-closed rewrite is entirely in the health
 * test, and the two cases that matter most (a CPU with NO RDRAND, and a clock
 * that does not actually move) cannot be summoned on demand from real silicon.
 * So tls_entropy.c is built here with -DTLS_ENT_HOSTTEST, which routes its
 * three CPU primitives through the hooks below; everything under that seam -
 * the workload, the health test, the SHA-256 conditioning, the probe order -
 * is the same code that ships.
 *
 * Scenarios (one per process: the source is cached in a file-static):
 *   rdrand    RDRAND present and working   -> source rdrand, draws differ
 *   jitter    no RDRAND, real clock        -> source jitter, draws differ
 *   deadrand  RDRAND advertised but always fails -> must fall through to jitter
 *   frozen    no RDRAND, clock never moves -> NO source, get_entropy refuses
 *   steplock  no RDRAND, clock ticks by a fixed step -> NO source
 *   coarse    no RDRAND, clock moves but takes 4 values -> NO source
 *
 * Build + run: tools/tls_entropy_test.sh
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../tls_entropy.h"

/* -- the environment tls_entropy.c sees ---------------------------------- */
static int  mode_has_rdrand;      /* what CPUID would report                 */
static int  mode_rdrand_works;    /* whether the instruction ever succeeds   */
static int  clock_mode;           /* 0 real, 1 frozen, 2 step-locked, 3 coarse */

unsigned net_tx_frames(void) { return 0; }
unsigned net_rx_frames(void) { return 0; }

static unsigned long long real_tsc(void)
{
    unsigned a, d;
    __asm__ volatile ("rdtsc" : "=a"(a), "=d"(d));
    return ((unsigned long long)d << 32) | a;
}

unsigned long long tls_ent_test_tsc(void)
{
    static unsigned long long t;
    switch (clock_mode) {
    case 1: return 0x1000;                 /* frozen: every delta is 0        */
    case 2: t += 4096; return t;           /* step-locked: every delta equal  */
    case 3: t += 4096 + ((t >> 12) & 3);   /* coarse: 4 distinct delta values */
            return t;
    default: return real_tsc();
    }
}

int tls_ent_test_has_rdrand(void) { return mode_has_rdrand; }

int tls_ent_test_rdrand(unsigned long long *out)
{
    if (!mode_rdrand_works) return 0;      /* CF=0: no value produced         */
    /* A stand-in DRNG. Its quality is not what this gate measures; what it
     * measures is that the PROBE trusts it only when it actually succeeds,
     * and that consecutive draws are not identical. */
    { static unsigned long long s = 0x243F6A8885A308D3ULL;
      s ^= real_tsc();
      s *= 0x9E3779B97F4A7C15ULL; s ^= s >> 29;
      *out = s; }
    return 1;
}

/* -- checks --------------------------------------------------------------- */
static int fails;
static void ck(const char *what, int ok)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

/* Four 32-byte draws must be pairwise distinct. One repeat out of six pairs is
 * ~2^-256 for a real source, so any repeat here is a dead source, not luck. */
static int draws_distinct(void)
{
    unsigned char d[4][32];
    int i, j;
    for (i = 0; i < 4; i++) if (!tls_entropy_get(d[i], 32)) return 0;
    for (i = 0; i < 4; i++)
        for (j = i + 1; j < 4; j++)
            if (!memcmp(d[i], d[j], 32)) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    const char *scen = argc > 1 ? argv[1] : "rdrand";
    int src;

    if (!strcmp(scen, "rdrand"))        { mode_has_rdrand = 1; mode_rdrand_works = 1; clock_mode = 0; }
    else if (!strcmp(scen, "jitter"))   { mode_has_rdrand = 0; mode_rdrand_works = 0; clock_mode = 0; }
    else if (!strcmp(scen, "deadrand")) { mode_has_rdrand = 1; mode_rdrand_works = 0; clock_mode = 0; }
    else if (!strcmp(scen, "frozen"))   { mode_has_rdrand = 0; mode_rdrand_works = 0; clock_mode = 1; }
    else if (!strcmp(scen, "steplock")) { mode_has_rdrand = 0; mode_rdrand_works = 0; clock_mode = 2; }
    else if (!strcmp(scen, "coarse"))   { mode_has_rdrand = 0; mode_rdrand_works = 0; clock_mode = 3; }
    else { fprintf(stderr, "unknown scenario %s\n", scen); return 2; }

    src = tls_entropy_source();
    printf("scenario %-9s source=%s\n", scen, tls_entropy_name());

    if (!strcmp(scen, "rdrand")) {
        ck("source is rdrand",                     src == TLS_ENT_RDRAND);
        ck("tls_have_rdrand() reports it",         tls_have_rdrand() == 1);
        ck("four draws are pairwise distinct",     draws_distinct());
        ck("selftest says live + non-repeating",   tls_entropy_selftest() == 1);
    } else if (!strcmp(scen, "jitter") || !strcmp(scen, "deadrand")) {
        /* deadrand is the important half: CPUID advertised a DRNG that never
         * succeeds, so the probe must NOT claim rdrand and must NOT give up -
         * it falls through to jitter, which is a real source. */
        ck("source is jitter",                     src == TLS_ENT_JITTER);
        ck("tls_have_rdrand() reports 0",          tls_have_rdrand() == 0);
        ck("four draws are pairwise distinct",     draws_distinct());
        ck("selftest says live + non-repeating",   tls_entropy_selftest() == 1);
    } else {
        /* The whole point of the rewrite: a clock that carries no entropy must
         * leave the box with NO source, and every draw must come back empty so
         * tls_connect refuses instead of seeding from it. */
        unsigned char buf[32];
        memset(buf, 0xA5, sizeof buf);
        ck("no usable source",                     src == TLS_ENT_NONE);
        ck("name is \"none\"",                     !strcmp(tls_entropy_name(), "none"));
        ck("tls_entropy_get refuses",              tls_entropy_get(buf, sizeof buf) == 0);
        ck("it wrote nothing into the buffer",     buf[0] == 0xA5 && buf[31] == 0xA5);
        ck("selftest reports no source (0)",       tls_entropy_selftest() == 0);
    }

    printf("%s: %s\n", scen, fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}

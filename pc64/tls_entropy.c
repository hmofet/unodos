/* ===========================================================================
 * UnoDOS/pc64 - entropy for TLS: RDRAND, else conditioned timing jitter,
 * else REFUSE.  Contract + rationale in tls_entropy.h.
 * ======================================================================== */
#include "tls_entropy.h"
#include "bearssl.h"
#include <string.h>

/* Link-level frame counters (net.c).  Declared locally rather than via net.h
 * so this file stays free of the stack's headers and the host gate can build
 * it alone; net.h's u32 is `unsigned int`, so the declarations agree. */
unsigned net_tx_frames(void);
unsigned net_rx_frames(void);

/* ---- raw CPU primitives -------------------------------------------------
 * The host gate (tools/tls_entropy_test.c) substitutes its own clock and its
 * own RDRAND answer so it can drive the DEAD-counter and no-RDRAND cases,
 * which are the two the health test exists for and which no real CPU offers
 * on demand.  Everything below this seam is identical in both builds. */
#ifdef TLS_ENT_HOSTTEST
unsigned long long tls_ent_test_tsc(void);          /* supplied by the gate  */
int  tls_ent_test_rdrand(unsigned long long *out);  /* 0 = "no RDRAND here"  */
int  tls_ent_test_has_rdrand(void);
#define ENT_TSC()        tls_ent_test_tsc()
#define ENT_RDRAND(p)    tls_ent_test_rdrand(p)
#define ENT_CPU_RDRAND() tls_ent_test_has_rdrand()
#else
static unsigned cpuid_ecx1(void)
{
    unsigned a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                              : "a"(1u), "c"(0u));
    return c;
}
static int rdrand64(unsigned long long *out)
{
    unsigned char ok;
    unsigned long long v;
    __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    *out = v;
    return ok;
}
static unsigned long long rdtsc(void)
{
    unsigned a, d;
    __asm__ volatile ("rdtsc" : "=a"(a), "=d"(d));
    return ((unsigned long long)d << 32) | a;
}
#define ENT_TSC()        rdtsc()
#define ENT_RDRAND(p)    rdrand64(p)
#define ENT_CPU_RDRAND() ((cpuid_ecx1() >> 30) & 1)
#endif

#define ENT_UNPROBED   (-1)
static int g_src = ENT_UNPROBED;
static int g_rdrand;                    /* 1 iff the CPU's DRNG is live       */

/* ---- the jitter source -------------------------------------------------- */
/* JIT_MEM must EXCEED L1 so the walk's memory latency - not the instruction
 * stream - dominates the measurement. Measured on the build host: at 4 KB / 64
 * steps the whole working set is L1-resident and the deltas take only 5-8
 * distinct low-5-bit values, so the health test below rejected a perfectly
 * good CPU about half the time. At 64 KB / 256 steps the same run yields 21-25
 * distinct values with no long runs, and costs ~1 us per sample. */
#define JIT_MEM        65536            /* bytes the timed workload walks     */
#define JIT_STEPS        256            /* memory touches per measurement     */
#define JIT_HEALTH       256            /* samples the health test consumes   */
#define JIT_MAXRUN        32            /* consecutive identical deltas = dead*/
#define JIT_PER_BLOCK    256            /* samples per 32-byte output block,
                                         * i.e. one conservatively-credited
                                         * bit each. ~1 us/sample, so a
                                         * 256-bit seed costs ~0.3 ms and the
                                         * one-off health test ~0.3 ms - both
                                         * taken before the TCP connect, so
                                         * neither eats a connect deadline. */

static unsigned char g_jitmem[JIT_MEM];
static unsigned      g_jitwalk;         /* walk position, carried across calls*/

/* One timed measurement.  Each step's index depends on the byte the previous
 * step read, so the compiler cannot hoist or vectorise the loop and the memory
 * system - not the instruction stream - sets the duration.  What we harvest is
 * cache/TLB state, DVFS transitions, SMIs, and whatever interrupt the firmware
 * timer takes underneath us. */
static unsigned long long jitter_delta(void)
{
    unsigned long long t0, t1;
    unsigned x = g_jitwalk, i;
    t0 = ENT_TSC();
    for (i = 0; i < JIT_STEPS; i++) {
        unsigned idx = (x + i * 61u) & (JIT_MEM - 1u);
        g_jitmem[idx] = (unsigned char)(g_jitmem[idx] + x + i);
        x += g_jitmem[idx] + 1u;
    }
    t1 = ENT_TSC();
    g_jitwalk = x;
    return t1 - t0;
}

/* Startup health test, in the spirit of SP800-90B's: the source counts only if
 * the deltas actually MOVE.  This rejects both ways a clock can be useless to
 * us - frozen or step-locked (long runs of identical deltas), and perfectly
 * regular (deltas that change but take only a handful of values).  Failing
 * here is the point: it turns "no usable RNG" into a refused handshake instead
 * of a predictable seed. */
static int jitter_healthy(void)
{
    unsigned long long prev = 0, d;
    unsigned seen = 0;                  /* bitmap of the low-5-bit values seen */
    int i, changes = 0, run = 0, maxrun = 0, bits = 0;
    for (i = 0; i < JIT_HEALTH; i++) {
        d = jitter_delta();
        if (i && d == prev) { if (++run > maxrun) maxrun = run; }
        else { run = 0; if (i) changes++; }
        seen |= 1u << (unsigned)(d & 31u);
        prev = d;
    }
    while (seen) { bits += (int)(seen & 1u); seen >>= 1; }
    if (maxrun >= JIT_MAXRUN)      return 0;   /* stuck / step-locked counter  */
    if (changes < JIT_HEALTH / 4)  return 0;   /* barely moves                 */
    if (bits < 8)                  return 0;   /* too few distinct values      */
    return 1;
}

/* Condition n bytes out of the jitter source.  Each 32-byte block is the
 * SHA-256 of (block counter || JIT_PER_BLOCK fresh samples || the link's frame
 * counters).  The frame counters are diversity only and are never credited -
 * they are externally observable - but they cost nothing and they fold in the
 * one piece of genuinely off-CPU timing pc64 has to hand. */
static void jitter_bytes(unsigned char *out, int n)
{
    br_sha256_context sh;
    unsigned char blk[32];
    unsigned ctr = 0;
    int off = 0;
    while (off < n) {
        int i, take = (n - off > 32) ? 32 : n - off;
        unsigned mix[2];
        br_sha256_init(&sh);
        br_sha256_update(&sh, &ctr, sizeof ctr);
        for (i = 0; i < JIT_PER_BLOCK; i++) {
            unsigned long long d = jitter_delta();
            br_sha256_update(&sh, &d, sizeof d);
        }
        mix[0] = net_tx_frames(); mix[1] = net_rx_frames();
        br_sha256_update(&sh, mix, sizeof mix);
        br_sha256_out(&sh, blk);
        memcpy(out + off, blk, (size_t)take);
        off += take; ctr++;
    }
    memset(blk, 0, sizeof blk);
    memset(&sh, 0, sizeof sh);
}

/* Decide the source once.  CPUID advertising RDRAND is not the same as RDRAND
 * WORKING: a failed DRNG returns CF=0 forever and some hypervisors advertise a
 * bit they do not back, so demand an actual success before trusting it. */
static int entropy_probe(void)
{
    unsigned long long v; int t, ok = 0;
    g_rdrand = ENT_CPU_RDRAND();
    if (g_rdrand) {
        for (t = 0; t < 64 && !ok; t++) ok = ENT_RDRAND(&v);
        if (ok) return TLS_ENT_RDRAND;
        g_rdrand = 0;                   /* advertised but dead - do not claim it */
    }
    return jitter_healthy() ? TLS_ENT_JITTER : TLS_ENT_NONE;
}

int tls_entropy_source(void)
{
    if (g_src == ENT_UNPROBED) g_src = entropy_probe();
    return g_src;
}

int tls_have_rdrand(void) { tls_entropy_source(); return g_rdrand; }

const char *tls_entropy_name(void)
{
    switch (tls_entropy_source()) {
    case TLS_ENT_RDRAND: return "rdrand";
    case TLS_ENT_JITTER: return "jitter";
    default:             return "none";
    }
}

int tls_entropy_get(unsigned char *out, int n)
{
    int i;
    switch (tls_entropy_source()) {
    case TLS_ENT_RDRAND:
        for (i = 0; i < n; i += 8) {
            unsigned long long v = 0; int t = 64;
            while (!ENT_RDRAND(&v) && t--) ;
            if (t < 0) { g_src = TLS_ENT_NONE; g_rdrand = 0; return 0; }  /* died mid-fill */
            memcpy(out + i, &v, (size_t)((n - i >= 8) ? 8 : (n - i)));
        }
        return 1;
    case TLS_ENT_JITTER:
        jitter_bytes(out, n);
        return 1;
    default:
        return 0;
    }
}

int tls_entropy_selftest(void)
{
    unsigned char a[32], b[32];
    int rc;
    if (!tls_entropy_get(a, sizeof a) || !tls_entropy_get(b, sizeof b)) rc = 0;
    else rc = memcmp(a, b, sizeof a) ? 1 : -1;
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    return rc;
}

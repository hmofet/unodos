/* cosmo64/entropy.c -- the three primitives pc64's tls_entropy.c asks a
 * platform for (TLS_ENT_PLATFORM). It supplies NO randomness of its own: the
 * jitter workload, the SP800-90B-flavoured health test and every refusal stay
 * in tls_entropy.c, where the security argument is made once for every target.
 * What lives here is a tick counter and an honest "no".
 *
 * THE HARDWARE DRNG: there isn't one. RNDR/RNDRRS arrived in ARMv8.5 and the
 * MT6771 is a Cortex-A73/A53 pair at ARMv8.0, so the instruction does not
 * exist -- reading it would be an undefined-instruction trap, not a zero. The
 * SoC very likely has a TRNG block somewhere in its I/O space, as MediaTek
 * parts generally do, but its address is not in this tree and nothing here has
 * ever read it; claiming a generator we have not proven is precisely the lie
 * tls_entropy.c says it cannot survive. So: has_rng() answers 0, the jitter
 * path is taken, and it counts only if the health test passes.
 *
 * THE TICK COUNTER, and the open question about it. CNTPCT_EL0 is what this
 * port already uses for every other kind of timing, and on this SoC CNTFRQ is
 * 13 MHz -- 77 ns per count. The jitter workload is a ~1 us dependent walk, so
 * a sample spans only tens of counts, and the health test wants the LOW FIVE
 * BITS of the deltas to take at least eight distinct values across 256
 * samples. Whether 13 MHz is fine enough for that is a measurement, not a
 * matter of opinion, so this file makes the measurement visible rather than
 * guessing: c64_entropy_report() prints the source the probe settled on, and
 * platform.c calls it during boot. If the answer turns out to be "too coarse",
 * the next lever is the PMU cycle counter (PMCCNTR_EL0, ~1.5 GHz), which is
 * deliberately NOT reached for first: enabling it means writing PMCR_EL0 from
 * EL2, and if a higher level has set MDCR_EL3.TPM that write traps out of our
 * world rather than returning an error. Do not take that risk for a counter we
 * have not yet shown we need.
 */

#include "cosmo64.h"

unsigned long long tls_ent_plat_ticks(void)
{
    return c64_cnt_now();               /* CNTPCT_EL0, 13 MHz on this SoC */
}

int tls_ent_plat_rng(unsigned long long *out)
{
    if (out) *out = 0;
    return 0;                           /* ARMv8.0: no RNDR, and no proven TRNG */
}

int tls_ent_plat_has_rng(void)
{
    return 0;
}

/* Boot-time visibility. The whole point of the fail-closed design is that a
 * machine with no usable source REFUSES to do TLS, and a refusal nobody can
 * explain is a bug report waiting to happen -- so the boot story says which
 * source the probe took, once, where readlog.sh and the URC replay both see
 * it. tls_entropy_selftest() is the gate's own hook (SPECTEST S-TLS-11): 1 =
 * live and two draws differed, 0 = no usable source, -1 = a source claimed to
 * be live and repeated itself. */
int tls_entropy_source(void);
const char *tls_entropy_name(void);
int tls_entropy_selftest(void);

void c64_entropy_report(void)
{
    int src = tls_entropy_source();
    int st  = tls_entropy_selftest();
    c64_logf("entropy: source=%s selftest=%d%s\n",
             tls_entropy_name(), st,
             src == 0 ? "  -- TLS WILL REFUSE (no usable source)" : "");
}

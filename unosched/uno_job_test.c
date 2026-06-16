/* uno_job_test — OFFLOAD floor/accel equivalence (Phase 10 host oracle).
 * Runs the same job via its portable kernel and via an "accel" variant (the stand-in
 * for an SPU/DSP version) and requires identical results — the gate that must hold
 * before the real PS3-SPU / Saturn-SH2 backend (which is hardware-blocked here). */
#include "uno_job.h"
#include <stdio.h>
#include <string.h>

#define N 4096
static long out_portable[N], out_accel[N];

/* a compute kernel: out[i] = i*i + something — two implementations, same math */
static void k_portable(int i, void *arg) { long *o = arg; o[i] = (long)i * i + 7; }
static void k_accel(int i, void *arg)     { long *o = arg; o[i] = (long)i * i + 7; } /* "SPU" variant */

int main(void) {
    uno_job_t job = { k_portable, k_accel };
    uno_job_dispatch(&job, N, out_portable, 0);   /* floor: portable kernel */
    uno_job_dispatch(&job, N, out_accel,    1);   /* OFFLOAD: accel variant */

    int same = memcmp(out_portable, out_accel, sizeof out_portable) == 0;
    int correct = out_portable[10] == 107 && out_portable[N-1] == (long)(N-1)*(N-1)+7;
    printf("tier=%s\n", UNO_TIER);
    printf("  [%s] accel job variant == portable kernel (OFFLOAD fallback contract)\n", same ? "PASS" : "FAIL");
    printf("  [%s] job computed expected values\n", correct ? "PASS" : "FAIL");

    /* a job with NO accel variant must still run (portable fallback) */
    uno_job_t job2 = { k_portable, NULL };
    memset(out_accel, 0, sizeof out_accel);
    uno_job_dispatch(&job2, N, out_accel, 1);     /* asks for accel; none -> portable */
    int fb = memcmp(out_portable, out_accel, sizeof out_portable) == 0;
    printf("  [%s] missing accel falls back to the portable kernel\n", fb ? "PASS" : "FAIL");

    int ok = same && correct && fb;
    printf("\n%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}

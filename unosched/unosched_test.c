/* unosched_test — the concurrency stress pass (CONTRACT-ARCH §10 / §15.2).
 *
 * The SAME workload compiled three ways:
 *   COOP            — inline/serial, locks vanish
 *   SMP             — pthreads, real mutex (built under ThreadSanitizer)
 *   SMP + UNO_RACE  — the mutex removed (the bug); TSan must REPORT a data race
 *
 * The runner (build.sh) asserts COOP result == SMP result == expected and that the
 * guarded SMP build is TSan-clean, while the racy build is caught. Here we just run
 * the workload and print the result + tier.
 */
#include "uno_sync.h"
#include <stdio.h>

#define N 100000
static long g_sum;
static uno_mutex_t g_lock = UNO_MUTEX_INIT;

/* job kernel: each index contributes i to a shared sum. The kernel guards the
 * shared structure with the sync primitive (real on SMP, free on COOP). */
static void add_job(int i, void *arg) {
    (void)arg;
#ifdef UNO_RACE
    g_sum += i;                       /* BUG: unguarded shared write -> TSan catches */
#else
    uno_lock(&g_lock);
    g_sum += i;
    uno_unlock(&g_lock);
#endif
}

int main(void) {
    (void)&g_lock;                    /* referenced even in the UNO_RACE build */
    g_sum = 0;
    uno_parallel_for(N, add_job, NULL);
    long expected = (long)N * (N - 1) / 2;
    int ok = (g_sum == expected);
    printf("tier=%-16s sum=%ld expected=%ld  [%s]\n", UNO_TIER, g_sum, expected, ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}

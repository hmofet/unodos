/* uno_parallel_for — inline on COOP, fanned out across cores on SMP. See uno_sync.h. */
#include "uno_sync.h"

#ifdef UNO_SMP
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

typedef struct { int lo, hi; void (*fn)(int, void *); void *arg; } range_t;
static void *worker(void *p) {
    range_t *r = p;
    for (int i = r->lo; i < r->hi; i++) r->fn(i, r->arg);
    return NULL;
}
void uno_parallel_for(int n, void (*fn)(int, void *), void *arg) {
    long nc = sysconf(_SC_NPROCESSORS_ONLN);
    int t = (nc < 1) ? 1 : (nc > 16 ? 16 : (int)nc);
    if (t > n) t = n;
    if (t < 1) return;
    pthread_t th[16]; range_t rg[16];
    int per = (n + t - 1) / t;
    for (int k = 0; k < t; k++) {
        rg[k].lo = k * per; rg[k].hi = (k + 1) * per > n ? n : (k + 1) * per;
        rg[k].fn = fn; rg[k].arg = arg;
        pthread_create(&th[k], NULL, worker, &rg[k]);
    }
    for (int k = 0; k < t; k++) pthread_join(th[k], NULL);
}
#else
void uno_parallel_for(int n, void (*fn)(int, void *), void *arg) {
    for (int i = 0; i < n; i++) fn(i, arg);     /* COOP: a parallel_for on a 6502 is a loop */
}
#endif

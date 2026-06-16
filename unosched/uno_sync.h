/* uno_sync — the portable concurrency API (CONTRACT-ARCH §10).
 *
 * One API, tiered backends selected at compile time:
 *   (default)   COOP floor — no preemption, so locks compile to NOTHING and
 *               uno_parallel_for runs inline/serial. Zero cost on tiny ports.
 *   -DUNO_SMP   pthreads — locks become real, parallel_for fans out across cores.
 *
 * App/kernel code uses the same calls everywhere: they vanish on a 6502 and become
 * real atomics/mutexes on a Saturn/PS3/host. The §10 correctness contract: code that
 * guards its shared state with these primitives is correct on every tier — verified
 * by running the SAME workload under COOP and under host-SMP+ThreadSanitizer and
 * requiring identical results + zero data races.
 */
#ifndef UNO_SYNC_H
#define UNO_SYNC_H
#include <stddef.h>

#ifdef UNO_SMP
#  include <pthread.h>
typedef pthread_mutex_t uno_mutex_t;
#  define UNO_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static inline void uno_lock(uno_mutex_t *m)   { pthread_mutex_lock(m); }
static inline void uno_unlock(uno_mutex_t *m) { pthread_mutex_unlock(m); }
static inline long uno_atomic_add(long *p, long v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
#  define UNO_TIER "SMP (pthreads)"
#else
typedef int uno_mutex_t;                      /* COOP: no preemption inside a yield-bounded section */
#  define UNO_MUTEX_INIT 0
static inline void uno_lock(uno_mutex_t *m)   { (void)m; }   /* compiles to nothing */
static inline void uno_unlock(uno_mutex_t *m) { (void)m; }
static inline long uno_atomic_add(long *p, long v) { long o = *p; *p += v; return o; }
#  define UNO_TIER "COOP (inline)"
#endif

/* a job kernel: called once per index in [0,n). On COOP it is a plain loop; on SMP
 * the range is partitioned across worker threads (uno3d's accel pattern, for compute). */
void uno_parallel_for(int n, void (*fn)(int i, void *arg), void *arg);

#endif /* UNO_SYNC_H */

/* uno_job — the OFFLOAD job model (CONTRACT-ARCH §10C, Phase 10).
 *
 * A job carries a PORTABLE C kernel that always exists, plus an OPTIONAL accelerated
 * variant the backend may select (an SPU/DSP version on PS3/Saturn). This is uno3d's
 * "swap at the highest altitude you can accelerate" applied to compute:
 *   - floor   : the portable kernel runs on the calling core (correct everywhere);
 *   - OFFLOAD : the accel variant runs on a specialized processor when present.
 * The contract that makes this safe: the accel variant must produce the SAME result
 * as the portable kernel (verified on host — the oracle for the real SPU/DSP).
 */
#ifndef UNO_JOB_H
#define UNO_JOB_H
#include "uno_sync.h"

typedef struct {
    void (*portable)(int i, void *arg);   /* always present — the floor */
    void (*accel)(int i, void *arg);       /* optional — selected on OFFLOAD-capable backends */
} uno_job_t;

/* dispatch a job over [0,n). use_accel requests the accel variant; if the job has
 * none, the portable kernel runs (the fallback always exists). Distribution uses
 * uno_parallel_for, so it is serial on COOP and parallel on SMP. */
static inline void uno_job_dispatch(const uno_job_t *job, int n, void *arg, int use_accel) {
    void (*fn)(int, void *) = (use_accel && job->accel) ? job->accel : job->portable;
    uno_parallel_for(n, fn, arg);
}

#endif /* UNO_JOB_H */

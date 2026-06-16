# unosched — tiered concurrency: the COOP floor + sync/job API (Phase 7, §10)

One portable API (`uno_sync.h`), tiered backends chosen at compile time:

- **COOP floor** (default) — no preemption, so `uno_lock/unlock` compile to nothing
  and `uno_parallel_for` runs inline/serial. Zero cost on tiny ports.
- **SMP** (`-DUNO_SMP`) — `pthreads`: real mutex; `uno_parallel_for` fans out across
  cores.

App/kernel code uses the same calls everywhere; they vanish on a 6502 and become
real atomics/mutexes on a Saturn/PS3/host.

## Host SMP + ThreadSanitizer oracle (the §10 correctness contract)

`sh unosched/build.sh` runs the **concurrency stress pass**: the SAME workload built
three ways and checked —

- **COOP** result `== ` **SMP** result `== ` expected (order-independent sum),
- the **guarded** SMP build is **TSan-clean**,
- a **`-DUNO_RACE`** build with the mutex removed is **caught by TSan** ("data race")
  — discrimination, so the clean pass isn't vacuous.

This is the cheap, deterministic oracle for Saturn/PS3 parallel correctness *before*
that hardware (Phase 10), exactly as the software rasteriser is the oracle for the
GPU backends. Note: TSan needs ASLR disabled on this kernel — `build.sh` runs the
instrumented binaries under `setarch -R`.

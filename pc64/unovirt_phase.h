/* ===========================================================================
 * unovirt - the phase tests, ABOVE the backend seam.
 *
 * A1..A6 are tests, and a test behind a vendor seam is a test written twice.
 * Every one of these was once an entry in `uno_hv_t`, which is why the SVM
 * backend could not reach A2 without reimplementing nine guests: the guests
 * are x86 machine code and the servicing is x86 register work, and neither of
 * those is vendor-specific.  They live in hv_phases.c now, compile once, and
 * reach the machine only through `uno_hv_t` (unovirt_hv.h).
 *
 * A phase declines rather than fails when the backend cannot host it - a
 * backend with no `map` has no second stage, so everything from A2 on says so
 * and the boot carries on.
 * ======================================================================== */
#ifndef UNO_VIRT_PHASE_H
#define UNO_VIRT_PHASE_H

#include "unovirt_hv.h"

/* What A4's guest reports about itself.  Each field is a separate claim with
 * its own way of being wrong, which is why this is a struct and not a bool. */
typedef struct uno_vm_clockirq {
    unsigned long long t1, t2;   /* its clock, sampled across two slices     */
    unsigned long long msr_echo; /* what it read back from an MSR we answered */
    int irqs;                    /* times its own handler ran                */
    int redelivered;             /* did the acknowledged one come back?      */
    int exits;                   /* how many exits it took to get there      */
} uno_vm_clockirq;

/* What A5's guest and its device report between them. */
typedef struct uno_vm_virtio {
    unsigned magic;          /* the identity register, read BY the guest    */
    unsigned used_idx;       /* what the guest read back out of the used ring */
    int bytes, notifies;     /* what the device consumed, and how often     */
    int cycle_refused;       /* a self-referential chain returned, not hung */
    const char *text;        /* what came through                           */
} uno_vm_virtio;

/* How far a real kernel got.  Not a pass/fail: the interesting outcomes are
 * all partial, and `stop_reason` plus the last line it printed is what makes
 * the next step obvious. */
typedef struct uno_vm_linux {
    long loaded;             /* bytes of kernel placed in the carve         */
    int  lines, chars;       /* what it said on its serial port             */
    const char *last;        /* ...and the last line of it                  */
    int  exits;
    unsigned stop_reason;    /* the exit that ended the run                 */
    unsigned long long stop_rip;
    unsigned fault_vec, fault_err;      /* 0xFFFF when it was not a fault   */
    unsigned long long fault_addr;
    int pio, pio_n;                     /* port I/O: how much, and where    */
    int injects;                        /* interrupts handed to the guest   */
    int shell_ok;                       /* the shell READ a line and replied */
    unsigned last_port;                 /* the one it is sitting on         */
    unsigned short pio_ports[8];
} uno_vm_linux;

/* A1: run the marker guest - it takes a value from us through a cpuid
 * intercept, writes it to its own memory, and halts.  1 when the value came
 * back out of GUEST memory, and *last carries the final exit either way. */
int uno_hvp_marker(const uno_hv_t *hv, unsigned long long want,
                   unsigned long long *got, uno_vmexit *last);

/* A1: a guest that destroys itself on purpose.  1 when the ruin was
 * CONTAINED - the machine is still ours and the exit says why. */
int uno_hvp_crasher(const uno_hv_t *hv, uno_vmexit *out);

/* A2: the same round trip, but through second-stage translation.  The guest
 * is placed in the carve at LOW guest-physical addresses and stores its marker
 * at `gpa`; the host reads it back at a completely different host-physical
 * address, which is the evidence.  *hpa receives that address so the report
 * can print the pair. */
int uno_hvp_ept(const uno_hv_t *hv, unsigned long long want,
                unsigned long long gpa, unsigned long long *got,
                unsigned long long *hpa, uno_vmexit *last);

/* A3: place a guest that never yields, then run it one budget at a time.
 * `slice` returns 0 when no such guest is placed.  Together these are how a
 * guest is scheduled on an OS with no scheduler: the frame loop hands it a
 * slice and the machine takes it back. */
int uno_hvp_spin_start(const uno_hv_t *hv);
int uno_hvp_slice(const uno_hv_t *hv, unsigned budget_us, uno_vmexit *out);

/* A4: the three things a guest needs before it can be an operating system -
 * a clock it can read, an interrupt it can take, and an MSR space somebody
 * answers.  One guest exercises all three. */
int uno_hvp_clockirq(const uno_hv_t *hv, uno_vm_clockirq *out);

/* A5: an MMIO device the guest discovers and talks to through a virtqueue.
 * This is the mechanism every later device is built on. */
int uno_hvp_virtio(const uno_hv_t *hv, uno_vm_virtio *out);

/* A6: load a bzImage into the carve, hand it a zero page, and place it. */
int uno_hvp_linux_boot(const uno_hv_t *hv, uno_vm_linux *out);

/* A6b: one budgeted slice of the kernel, from the frame loop.  A boot needs
 * seconds and a selftest cannot give them; this is where a guest actually
 * lives.  Returns 0 once the kernel has stopped. */
int uno_hvp_linux_slice(const uno_hv_t *hv, unsigned budget_us,
                        uno_vm_linux *out);

#endif /* UNO_VIRT_PHASE_H */

/* ===========================================================================
 * UnoDOS/pc64 - UNOAUTOMATE core (unoauto.c): systemwide logging, probing,
 * and automation.  The successor to the ad-hoc UNO_DEBUG harness.
 *
 * WHAT THIS IS
 *   One subsystem that can observe and drive the whole OS:
 *     - LOG      structured, channelled logging every subsystem feeds
 *     - TEST     the registered-suite runner (absorbs SPECTEST/nettest)
 *     - PROBE    peek into processes/threads/modules (state, stacks, counters)
 *     - HOOK     intercept a subsystem's call surface for tracing/fuzzing
 *     - DRIVE    inject UI events + call app/OS APIs, scriptable from Python
 *                (via the PYRT PyHost - see pyhost.h)
 *
 * MIGRATION CONTRACT (the seam)
 *   Stage 1 (this file): the unoauto_* API lands; pc64_nettest.c /
 *   pc64_spectest.c / uno_debug.c internals delegate INTO it.  Every legacy
 *   uno_dbg_* entry point in uno_debug.h keeps working as a thin wrapper -
 *   callers (drivers, apps) do not change at the seam.
 *   Stage 2: PROBE/HOOK/DRIVE build out in new files behind this API.
 *   Stage 3: the legacy files dissolve; wrappers remain until the last
 *   caller is migrated, then uno_debug.h shrinks to an #include of this.
 *
 *   Compiled when -DUNO_DEBUG (same gate as the old harness).  In a
 *   production build every hook compiles away, exactly like uno_debug.h.
 * ======================================================================== */
#ifndef UNOAUTO_H
#define UNOAUTO_H

#ifdef UNO_DEBUG

/* ---- LOG: channelled structured logging --------------------------------- */
typedef enum {
    UA_CH_KERNEL = 0,   /* boot, memory, faults        (was uno_dbg_log)      */
    UA_CH_NET,          /* drivers + stack             (was uno_dbg_net_trace)*/
    UA_CH_UI,           /* shell, unoui, input                                */
    UA_CH_STORAGE,      /* fat, blkdev, install                               */
    UA_CH_TEST,         /* suite runner output                                */
    UA_CH_SCRIPT,       /* Python automation           (PYRT)                 */
    UA_CH_COUNT
} UnoAutoChan;

void unoauto_log(UnoAutoChan ch, const char *fmt, ...);
void unoauto_vlog(UnoAutoChan ch, const char *fmt, __builtin_va_list ap);

/* A sink receives every line of the channels in `mask` (bit N = channel N).
 * Sinks are how CRASH\NETLOG.TXT, the kernel ring, QEMU debugcon, and future
 * consumers (live UI console, script capture) attach without the producers
 * knowing.  Returns a sink id, or -1 when the table is full. */
typedef void (*UnoAutoSinkFn)(UnoAutoChan ch, const char *line, void *user);
int  unoauto_sink_add(unsigned mask, UnoAutoSinkFn fn, void *user);
void unoauto_sink_remove(int id);

/* ---- TEST: the registered suite runner ----------------------------------
 * SPECTEST's suites are registrations against this table instead of clauses
 * inside one monolithic run function - enumerable, individually runnable
 * (the future Python `unoauto.test.run("storage")`).  ctx is per-run scratch
 * handed to every fn; return 0 = pass, >0 = the number of failed checks
 * inside the suite (detail lines stay the suite's own job - the runner
 * brackets each id with uno_dbg_check and logs one result line per fn on
 * the TEST channel). */
typedef int (*UnoAutoTestFn)(void *ctx);
int  unoauto_test_register(const char *suite, const char *id, UnoAutoTestFn fn);
int  unoauto_test_run(const char *suite_or_null, void *ctx, char *report, int cap);

/* ---- PROBE: observe processes/threads/modules (Stage 2) ------------------
 * The one enumeration surface for "what is the system doing":  loaded .UNO
 * modules, live UnoUuiApp instances, the main-loop phase, per-window draw
 * costs, net-stack socket states.  Snapshot-based - fill caller memory, no
 * locks held across the call. */
typedef struct {
    const char *name;       /* module/app/subsystem name                      */
    int         kind;       /* 0 module  1 app  2 subsystem                   */
    int         state;      /* kind-specific (running/blocked/idle)           */
    unsigned long long busy_cyc;   /* TSC spent since the last snapshot       */
} UnoAutoProbeEnt;
int unoauto_probe(UnoAutoProbeEnt *out, int max);

/* ---- HOOK: intercept a call surface (Stage 2) ----------------------------
 * Named tap points (e.g. "net.tx", "fat.write", "uui.action") that a script
 * or test can attach to for tracing, latency accounting, or fault
 * injection.  The oom-every-Nth-malloc knob becomes a hook client. */
typedef void (*UnoAutoHookFn)(const char *point, void *arg, void *user);
int  unoauto_hook_add(const char *point, UnoAutoHookFn fn, void *user);
void unoauto_hook_remove(int id);
void unoauto_hook_fire(const char *point, void *arg);   /* producers call     */

/* ---- DRIVE: scripted automation (Stage 2) --------------------------------
 * UI event injection reuses uno_pc64_inject_key/_pointer (uno_debug.h).
 * The Python binding surfaces this whole header as an `unoauto` module via
 * the PYRT PyHost (pyhost.h): scripts enumerate probes, attach hooks, drive
 * apps, and assert on LOG output - the systemwide automation story. */
int  unoauto_drive_ready(void);          /* 1 when PYRT + shell are up        */

#else /* !UNO_DEBUG: everything compiles away */
#define unoauto_log(...)                 ((void)0)
#define unoauto_sink_add(m, f, u)        (-1)
#define unoauto_sink_remove(i)           ((void)0)
#define unoauto_test_register(s, i, f)   (-1)
#define unoauto_test_run(s, x, r, c)     (-1)
#define unoauto_probe(o, m)              0
#define unoauto_hook_add(p, f, u)        (-1)
#define unoauto_hook_remove(i)           ((void)0)
#define unoauto_hook_fire(p, a)          ((void)0)
#define unoauto_drive_ready()            0
#endif

#endif /* UNOAUTO_H */

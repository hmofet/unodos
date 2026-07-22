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

/* ---- THE CONTRACT ---------------------------------------------------------
 * This header is the interface contract between unoautomate and every other
 * agent/subsystem building against it.  Each section below is marked:
 *
 *   [STABLE]        name, signature and semantics are FROZEN.  A breaking
 *                   change to a STABLE item must (a) ship the new name
 *                   alongside the old one, with the old kept as a deprecated
 *                   wrapper for one transition window, (b) bump UNOAUTO_API,
 *                   (c) get a dated entry in HARNESS-POLICY.md "API
 *                   changelog".  Names are never repurposed with different
 *                   semantics.
 *   [EXPERIMENTAL]  may change or disappear between rebases without a
 *                   version bump.  Usable, but re-read this header after
 *                   every pull before relying on one.
 *
 * The legacy uno_dbg_* entry points (uno_debug.h) are wrappers over this
 * core and inherit [STABLE].  If UNOAUTO_API moved after a pull, read the
 * changelog before building. */
#define UNOAUTO_API 1

/* ---- HOOK event payloads (outside the UNO_DEBUG gate so production call
 * sites still typecheck; the fire itself compiles away).  One struct per
 * tap-point family; the registered points and their payloads:
 *
 *   "libc.malloc"        UnoAutoAllocEv   set .fail=1 to make it return 0
 *   "fs.read" "fs.write" UnoAutoFsEv      trace-only
 *   "mod.load" "mod.unload"  UnoAutoModEv trace-only (.ok = outcome)
 *   "net.tx" "net.rx"    (requested from the net owner - see
 *                         UNOAUTOMATE-REQUESTS.md)
 *   "uui.action"         lands with DRIVE (Phase D)
 *
 * Hook fns run synchronously inside the producer and MUST NOT allocate
 * (the malloc tap is re-entrancy-guarded, but do not lean on it). */
typedef struct { unsigned long size; int fail; } UnoAutoAllocEv;
typedef struct { int vol; const char *path; long len; } UnoAutoFsEv;
typedef struct { const char *file; int ok; } UnoAutoModEv;

#ifdef UNO_DEBUG

/* ---- LOG: channelled structured logging ------------------------ [STABLE] */
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

/* ---- TEST: the registered suite runner ------------------------- [STABLE]
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

/* Wall-clock budget [EXPERIMENTAL] (UNOAUTOMATE-REQUESTS 2026-07-22): give
 * every test in subsequent unoauto_test_run calls a per-test budget.  A test
 * that returns AFTER its budget is force-recorded FAIL (even if it passed)
 * with an OVERRAN line, and the run continues - a flaky-slow live check
 * fails itself, not the whole gate.  0 = off (the default).
 *   A synchronous runner cannot preempt a test blocked INSIDE one call; for
 * that, long-running ops poll unoauto_deadline_left_ms() from their wait
 * loops and bail when it hits 0 (-1 = no deadline armed, run free). */
void unoauto_test_deadline_ms(unsigned ms);
long unoauto_deadline_left_ms(void);

/* ---- PROBE: observe processes/threads/modules (Stage 2) -- [EXPERIMENTAL]
 * The one enumeration surface for "what is the system doing".  Snapshot-
 * based: one call fills caller memory, no locks held across it.  Rows:
 *
 *   kind 0  MODULE     name=.UNO file      state=present on a volume
 *   kind 1  WINDOW     name=window title   state=1 focused
 *                      v1=total draw TSC   v2=worst draw us  (F11 profiler)
 *   kind 2  SUBSYSTEM  name=               state=            v1= / v2=
 *            "heap"                        0                 used / free
 *            "net"                         b0 link b1 lease  tx / rx frames
 *            "fs"                          volume count      writable count
 *            "shell"                       open windows      uptime ms
 *
 * name pointers are stable (string literals / static tables). */
typedef struct {
    const char *name;
    int         kind;       /* 0 module  1 window  2 subsystem                */
    int         state;      /* kind-specific, see the table above             */
    unsigned long long v1, v2;     /* kind-specific detail                    */
} UnoAutoProbeEnt;
int unoauto_probe(UnoAutoProbeEnt *out, int max);   /* rows filled */

/* ---- HOOK: intercept a call surface (Stage 2) ------------ [EXPERIMENTAL]
 * Named tap points (e.g. "net.tx", "fat.write", "uui.action") that a script
 * or test can attach to for tracing, latency accounting, or fault
 * injection.  The oom-every-Nth-malloc knob becomes a hook client. */
typedef void (*UnoAutoHookFn)(const char *point, void *arg, void *user);
int  unoauto_hook_add(const char *point, UnoAutoHookFn fn, void *user);
void unoauto_hook_remove(int id);
void unoauto_hook_fire(const char *point, void *arg);   /* producers call     */

/* ---- DRIVE: scripted automation (Stage 2) ---------------- [EXPERIMENTAL]
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
#define unoauto_test_deadline_ms(ms)     ((void)(ms))
#define unoauto_deadline_left_ms()       (-1L)
#define unoauto_probe(o, m)              0
#define unoauto_hook_add(p, f, u)        (-1)
#define unoauto_hook_remove(i)           ((void)0)
/* consumes its args: production tap sites must not warn set-but-unused */
#define unoauto_hook_fire(p, a)          ((void)(p), (void)(a))
#define unoauto_drive_ready()            0
#endif

#endif /* UNOAUTO_H */

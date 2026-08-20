/* ===========================================================================
 * UnoDOS/pc64 - production fallbacks for the debug-only kernel hooks that
 * unoautomate calls.
 *
 * unoautomate grew up inside the UNO_DEBUG harness, so its core reaches for
 * things only that harness provides: the kernel log ring, checkpoints, the
 * uptime clock, the dead-man's-switch watchdog, the DEBUG.CFG reader, the
 * NETLOG sink, the draw profiler.  Now that unoauto ships in production
 * (unoauto_gate.h) those call sites still exist in an image that has no debug
 * harness at all.
 *
 * Rather than #ifdef every call site - which would fork unoauto into two
 * versions and make the debug build the only one anybody tests - this file
 * supplies the missing definitions for a PRODUCTION build only.  The whole file
 * is `#ifndef UNO_DEBUG`, so a debug build compiles it to nothing and links the
 * real implementations in uno_debug.c / pc64_stress.c / pc64_nettest.c exactly
 * as before.
 *
 * WHY NOT WEAK SYMBOLS, the tree's usual trick (r8169_dbg_cmd, devmgr_list_str,
 * uno_hw_wdt_cmd)?  Those work because the weak definition and its callers sit
 * in the SAME translation unit.  Across objects, mingw lowers a weak definition
 * to a COFF weak external (the symbol table shows `.weak.uno_dbg_log.`), and ld
 * does not use the alternate to satisfy a reference from a DIFFERENT object -
 * the link fails with a plain `undefined reference to uno_dbg_log`.  That was
 * tried first; do not "simplify" this file back to weak.
 *
 * "Honest" is doing some work in that sentence.  Each fallback below either
 * does the real thing by another route (uptime), or reports absence in a way
 * the caller can act on (the guard, the config reader) - never a lie that makes
 * a production build silently behave as if a debug facility were present.
 * ======================================================================== */
#ifndef UNO_DEBUG          /* a debug build has the real thing - see above */

#include "unoauto.h"

long TickCount(void);          /* 60 Hz monotonic (mac_compat.h) */

/* ---- kernel log ring (uno_debug.c) ----------------------------------------
 * There is no ring in production.  unoauto's own sink table is the real log
 * path now - the ring was only ever sink slot 0 - so dropping the line here
 * loses nothing a production image had. */
void uno_dbg_log(const char *fmt, ...) { (void)fmt; }

/* ---- checkpoints (uno_debug.c) --------------------------------------------
 * A checkpoint tags "where were we" for a crash/hang report.  Production files
 * no reports, so there is nothing to tag. */
void uno_dbg_check(const char *tag) { (void)tag; }

/* ---- uptime (uno_debug.c) -------------------------------------------------
 * This one has to keep WORKING, not just link: the URC `uptime` verb, the test
 * runner's wall-clock budget and the screen recorder's frame clock all read it.
 * TickCount is the production monotonic clock (60 Hz, same base unosecure uses
 * for escalation lifetimes), so convert rather than return 0.  Coarser than the
 * debug build's TSC-derived clock - 16.7 ms granularity - which is fine for
 * every consumer here. */
unsigned long long uno_dbg_uptime_ms(void)
{
    return (unsigned long long)TickCount() * 1000ull / 60ull;
}

/* ---- host-attested guard (uno_debug.c) ------------------------------------
 * The dead-man's switch is the debug watchdog ISR: arm it before a risky verb
 * and a wedge resets the box instead of stranding it.  Production has no
 * watchdog and no fault handler to fire from, so the guard cannot exist.
 * uno_dbg_guard_armed() answering 0 is what makes the `guard`/`pet`/`safe`
 * verbs report "not-armed" to the operator - they are told the safety net is
 * absent rather than being handed a fake one. */
void uno_dbg_guard_arm(unsigned timeout_ms) { (void)timeout_ms; }
void uno_dbg_guard_pet(void)   { }
void uno_dbg_guard_clear(void) { }
int  uno_dbg_guard_armed(void) { return 0; }

/* ---- draw profiler (uno_debug.c) ------------------------------------------
 * Per-window draw cost, reported in PROBE rows.  No profiler in production, so
 * the rows come back without timing detail (the probe fills v1/v2 with 0). */
int uno_dbg_win_stat(int i, const char **title,
                                           unsigned long long *cyc,
                                           unsigned long *max_us)
{ (void)i; (void)title; (void)cyc; (void)max_us; return 0; }

/* ---- A/B frame instrumentation (uno_debug.c) -------------------------------
 * The probe's perf row (frame count + TSC scale) and the per-slot call counts
 * exist so a host can turn draw cycles into ms/frame.  Production has no draw
 * profiler and no calibrated TSC scale, so all three answer 0: the perf row
 * reads as "no data", the same honesty rule as the guard above, and the
 * kind-3 profile loop terminates immediately because uno_dbg_win_stat already
 * returns 0 rows here. */
unsigned long      uno_dbg_frames(void)      { return 0; }
unsigned long      uno_dbg_win_calls(int i)  { (void)i; return 0; }
unsigned long long uno_dbg_tsc_per_ms(void)  { return 0; }

/* NOTE: uno_heap_stats is NOT here.  pc64_libc.c already ships its own
 * production stub (zeros) beside the debug walker, so defining one here is a
 * duplicate-symbol link error, not a fallback. */

/* ---- DEBUG.CFG reader (pc64_stress.c) -------------------------------------
 * There is no DEBUG.CFG on a production stick - that file IS the "this is a
 * test image" marker.  Reporting every key absent is what makes
 * unoauto_remote_boot fall through to the gate: no `listen`, no `remote=`, no
 * `discover`, so a production image never brings the channel up on its own.
 * Arming is the only way in, which is the entire point. */
int pc64_stress_cfg_flag(const char *key)
{ (void)key; return 0; }

int pc64_stress_cfg_value(const char *key, char *buf, int cap)
{ (void)key; if (buf && cap > 0) buf[0] = 0; return 0; }

/* ---- NETLOG persistence sink (pc64_nettest.c) -----------------------------
 * CRASH\NETLOG.TXT is a debug-harness artifact; production writes no telemetry
 * to the boot volume.  unoauto's core calls this at first-log to make sure the
 * sink is registered before any NET line; with no sink to register it is a
 * no-op and NET lines just flow to whatever sinks are attached (the remote
 * link, typically). */
void pc64_netlog_sink_ensure(void) { }

#else
/* A debug build compiles this file to nothing.  ISO C wants a declaration. */
typedef int unoauto_compat_unused_in_debug_build;
#endif /* !UNO_DEBUG */

/* ===========================================================================
 * UnoDOS/pc64 DEBUG BUILD core (uno_debug.c) - crash reports, watchdog,
 * kernel log, boot environment block, perf HUD counters, fault injection.
 *
 * Compiled ONLY when -DUNO_DEBUG (build.sh on the pc64-debug-stress branch
 * defaults it on).  Everything here exists to turn a silent metal failure -
 * a triple-fault reboot, a hard freeze, a slow-but-working UI - into a plain
 * ASCII report in CRASH\ on the boot volume that a later agent can read.
 *
 * Report families (all plain text, 8.3 names, on the FAT32 boot volume):
 *   CRASH\CR###.TXT   CPU exception (regs + backtrace + log tail + env block)
 *   CRASH\HG###.TXT   watchdog: the main loop stalled (last checkpoint + log)
 *   CRASH\RS###.TXT   residue: a previous boot died without writing a report
 *                     (triple fault / power event) but its RAM log survived
 *   CRASH\PF###.TXT   periodic perf snapshot written by the stress driver
 *   BOOTENV.TXT       the boot environment block of the LAST boot (always)
 *
 * Persistence order on a crash: (1) a warm-reset-surviving RAM stash,
 * (2) the FAT volume when the transport is safe to touch from the fault
 * context, (3) QEMU debugcon when -DUNO_DBGCON.  The next boot flushes any
 * stashed report to disk before anything else can crash again.
 * ======================================================================== */
#ifndef UNO_DEBUG_H
#define UNO_DEBUG_H

#ifdef UNO_DEBUG

/* ---- bring-up (called from uefi_main.c) --------------------------------- */
void uno_dbg_early(void *SystemTable);  /* pre-GOP: stash + log ring + IDT hook */
void uno_dbg_envblock(void);            /* after init: build the env block      */
void uno_dbg_flush_residue(void);       /* after uno_fat_init: persist stashed
                                           reports/logs from the previous boot  */
void uno_dbg_watchdog_start(void);      /* fw timer event (attached)            */
void uno_dbg_on_detach(void);           /* own IDT + LAPIC watchdog (detached)  */

/* ---- kernel log ring (also mirrored into the RAM stash) ----------------- */
void uno_dbg_log(const char *fmt, ...);
void uno_dbg_check(const char *tag);    /* checkpoint: named marker + TSC; the
                                           watchdog/hang report shows the last
                                           one, so bracket risky waits with it */
void uno_dbg_heartbeat(void);           /* main loop liveness (once per frame)  */

/* ---- crash/panic entry points ------------------------------------------- */
void uno_dbg_panic(const char *why);    /* software-detected fatal (canary,
                                           assert): report + reset, no return  */
void uno_dbg_note(const char *fmt, ...);/* non-fatal anomaly: logged + counted,
                                           surfaced in perf snapshots          */

/* UNO_ASSERT(cond, "S-XXX-NN what"): the [assert]-tagged SPEC contracts, live.
 * A violated invariant is logged + counted via uno_dbg_note (non-fatal - a
 * spurious assert must never take down a test run), and shows up as notes>0
 * in the perf snapshot with the contract id in the boot log. Compiles to
 * nothing in a non-debug build, so it is free in the shipped OS. */
#define UNO_ASSERT(cond, id) \
    do { if (!(cond)) uno_dbg_note("ASSERT %s (%s:%d)", (id), __FILE__, __LINE__); } while (0)

/* ---- perf HUD counters (fed from the main loop / present path) ----------
 * The render/present feeds take raw TSC cycle deltas (rdtsc at both ends);
 * conversion to time happens here where the calibrated rate lives. */
void uno_dbg_frame_render_cyc(unsigned long long cyc);
void uno_dbg_frame_present_cyc(unsigned long long cyc);
void uno_dbg_frame_idle(int was_idle);  /* 1 = frame slept, 0 = worked         */
int  uno_dbg_hud(char *buf, int max);   /* one-line HUD text; 0 = HUD off      */
int  uno_dbg_perf_line(char *buf, int max);  /* perf numbers for the PF report */
void uno_dbg_hud_toggle(void);

/* ---- fault injection ----------------------------------------------------- */
extern int uno_dbg_oom_every;           /* fail every Nth malloc (0 = off)     */
int  uno_dbg_oom_tick(void);            /* called by malloc; 1 = fail this one */
void uno_dbg_force(int what);           /* test hooks: 0 #PF  1 #UD  2 #DE
                                           3 stack smash  4 hang (never rets)
                                           5 uno_dbg_panic                     */

/* ---- state the stress driver / shell reads ------------------------------- */
int  uno_dbg_crash_count(void);         /* CRASH\ report files seen at boot    */
unsigned long long uno_dbg_uptime_ms(void);
const char *uno_dbg_build_id(void);
void uno_dbg_mark_clean(void);          /* deliberate shutdown/restart         */
void uno_dbg_write_bootenv(void);       /* BOOTENV.TXT                         */
void uno_dbg_write_bootlog(void);       /* CRASH\BOOTLOG.TXT - every boot      */
void uno_dbg_boot_marker(void);         /* CRASH\BOOTS.TXT - earliest proof    */
void uno_dbg_write_perf(const char *text, int len);  /* CRASH\<M>\PF###.TXT    */
int  uno_dbg_write_crashfile(const char *name, const void *data, int len);
int  pc64_stress_cfg_flag(const char *key);  /* STRESS.CFG key set? -1 = no file */
int  pc64_stress_cfg_value(const char *key, char *buf, int cap); /* value after key= */
/* SMBIOS-derived machine tag = the telemetry folder leaf (CRASH\<TAG>\): one
 * stick serves a whole batch of machines without results colliding. */
const char *uno_dbg_machine_tag(void);

/* ---- per-window draw profiler (F11) --------------------------------------
 * Registered as unoui_profile_win by the shell; PF snapshots carry the top
 * window costs, the worst-frame line names the culprit window, and a hang
 * inside a draw callback names its window in the HG report. */
void uno_dbg_win_profile(const char *title, int begin);
void uno_dbg_win_frame_reset(void);
const char *uno_dbg_current_window(void);
unsigned long uno_dbg_cyc_to_us(unsigned long long cyc);  /* 0 if uncalibrated */
unsigned long long uno_dbg_tsc_per_ms(void);              /* 0 if uncalibrated */
int uno_pc64_mtrr_wc_experiment(void);   /* P3 opt-in (uefi_main); -1 = refused */

/* ---- the stress driver (pc64_stress.c) ---------------------------------- */
void pc64_stress_tick(void);            /* call once per main-loop frame       */
void pc64_stress_stop(void);            /* F12: disarm + hand back the desktop */
const char *pc64_stress_status(void);   /* on-screen run state, 0 when unarmed */

/* ---- the network hardware test (pc64_nettest.c) --------------------------
 * Runs once from the shell main loop, before the stress driver arms: USB
 * ethernet if an adapter is present at boot, else WiFi with a full bring-up
 * trace. Progressive log to CRASH\NETLOG.TXT. */
void pc64_nettest_tick(void);           /* one-shot; later calls are free      */
void pc64_spectest_run(void);           /* SPECTEST conformance suite (spec)   */
/* Interactive conformance: wait up to timeout_ms for a REAL keystroke (proves
 * the physical keyboard path). 1 = key (scan/uni set), 0 = timed out. */
int  uno_pc64_dbg_key_wait(int timeout_ms, int *scan, int *uni);
/* Trace sink for the network drivers (iwlwifi/wpa/usb-eth): appends a line to
 * the NETLOG buffer, mirrors it to the kernel log, and FEEDS THE WATCHDOG
 * HEARTBEAT - WiFi bring-up legitimately spends >20 s in scan+join, and a
 * trace that reset the machine mid-join would destroy the evidence it exists
 * to collect. Safe to call outside the net test (before the shell: buffered). */
void uno_dbg_net_trace(const char *fmt, ...);

/* ---- boot test phase on-screen progress (pc64_nettest.c) ------------------
 * The whole boot test phase (mtrr experiment -> SPECTEST -> net test) runs
 * synchronously inside one shell tick: the desktop is up but input and
 * repaint are BLOCKED for a minute or more, which reads as a hang (a Yoga
 * operator pulled the disk mid-run). These paint a live one-line banner from
 * inside the blocking code so the screen always names what is running.
 * progress() draws + presents immediately; progress_done() hands the strip
 * back to the shell for repaint. No-ops before the fb exists. */
void uno_dbg_progress(const char *fmt, ...);
void uno_dbg_progress_done(void);

/* ---- synthetic input (uefi_main.c, UNO_DEBUG only) ----------------------- */
void uno_pc64_inject_key(int scan, int uni, int ctrl);
void uno_pc64_inject_pointer(int x, int y, int btn);

#else /* !UNO_DEBUG: every hook compiles away */
#define uno_dbg_early(st)            ((void)0)
#define uno_dbg_envblock()           ((void)0)
#define uno_dbg_flush_residue()      ((void)0)
#define uno_dbg_watchdog_start()     ((void)0)
#define uno_dbg_on_detach()          ((void)0)
#define uno_dbg_log(...)             ((void)0)
#define uno_dbg_check(tag)           ((void)0)
#define uno_dbg_heartbeat()          ((void)0)
#define uno_dbg_note(...)            ((void)0)
#undef  UNO_ASSERT
#define UNO_ASSERT(cond, id)         ((void)0)
#define uno_dbg_frame_render_cyc(c)  ((void)0)
#define uno_dbg_frame_present_cyc(c) ((void)0)
#define uno_dbg_frame_idle(i)        ((void)0)
#define uno_dbg_oom_tick()           0
#define uno_dbg_mark_clean()         ((void)0)
#define uno_dbg_boot_marker()        ((void)0)
#define pc64_stress_tick()           ((void)0)
#define pc64_nettest_tick()          ((void)0)
#define uno_dbg_net_trace(...)       ((void)0)
#define uno_dbg_progress(...)        ((void)0)
#define uno_dbg_progress_done()      ((void)0)
#define uno_dbg_win_frame_reset()    ((void)0)
#define uno_dbg_cyc_to_us(c)         0ul
#endif

#endif /* UNO_DEBUG_H */

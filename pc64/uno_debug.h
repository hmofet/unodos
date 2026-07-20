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
void uno_dbg_write_bootenv(void);       /* BOOTENV.TXT (stress driver, once)   */
void uno_dbg_write_perf(const char *text, int len);  /* CRASH\PF###.TXT        */

/* ---- the stress driver (pc64_stress.c) ---------------------------------- */
void pc64_stress_tick(void);            /* call once per main-loop frame       */
void pc64_stress_stop(void);            /* F12: disarm + hand back the desktop */
const char *pc64_stress_status(void);   /* on-screen run state, 0 when unarmed */

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
#define uno_dbg_frame_render_cyc(c)  ((void)0)
#define uno_dbg_frame_present_cyc(c) ((void)0)
#define uno_dbg_frame_idle(i)        ((void)0)
#define uno_dbg_oom_tick()           0
#define uno_dbg_mark_clean()         ((void)0)
#define pc64_stress_tick()           ((void)0)
#endif

#endif /* UNO_DEBUG_H */

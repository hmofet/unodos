/* ===========================================================================
 * unodevices - PCH TCO HARDWARE watchdog (uno_hw_wdt.c).  See HWWATCHDOG.md.
 *
 * A last-resort dead-man's switch built from SEPARATE SILICON: the Intel PCH
 * TCO timer counts down in the chipset and resets the board when it reaches
 * zero, with NO dependence on the CPU still taking interrupts or the OS main
 * loop still cycling.  That is the one thing the software guard in uno_debug.c
 * (main-loop heartbeat / LAPIC-ISR / UEFI SetWatchdogTimer) can NOT do: all
 * three of those need the CPU alive.  The wedge class none of them catch - a
 * tight `cli; for(;;){}` spin, or a true bus hang - is exactly what this backs
 * stops.  See REMOTE.md "The guard" and the request in UNOAUTOMATE-REQUESTS.md.
 *
 * unoautomate arms/pets/disarms this ALONGSIDE the software guard (weak-symbol
 * seam, the r8169_dbg_cmd pattern - unoautomate declares a weak stub locally so
 * the tree links green before/after this strong symbol lands).  The TCO timeout
 * is set to the guard timeout PLUS a margin, so the software paths always get
 * first crack and the TCO is the true backstop, not a competitor.
 *
 * Compiled ONLY under -DUNO_DEBUG (matching the rest of the harness); every
 * entry point compiles to a no-op in the shipped OS, so this is free in prod.
 *
 * HONESTY CONTRACT: uno_hw_wdt_present() returns 1 only after a usable TCO was
 * located AND its NO_REBOOT bit was cleared *and read back clear*.  A firmware
 * that owns the TCO, or a chipset whose NO_REBOOT location this driver does not
 * (yet) know how to reach, yields present()==0 - the driver never pretends to
 * guard a box it cannot actually reset.  See HWWATCHDOG.md "Coverage".
 * ======================================================================== */
#ifndef UNO_HW_WDT_H
#define UNO_HW_WDT_H

/* Bumped on any breaking change to the four-call surface below, with a dated
 * HWWATCHDOG.md changelog entry (AGENTS.md §6).  1 = the initial TCO primitive. */
#define UNO_HW_WDT_API 1

#ifdef UNO_DEBUG

/* 1 iff a usable TCO was found, its NO_REBOOT bit cleared+verified, and the
 * timer is arm-able; 0 otherwise (no TCO, firmware-locked, or an unsupported
 * NO_REBOOT generation).  Lazily runs one-time discovery on first call.  Safe
 * to call repeatedly; the discovery result is cached. */
int  uno_hw_wdt_present(void);

/* Start (or restart) the hardware countdown so the board hard-resets in about
 * `seconds` if it is never petted again.  Because a classic TCO reboots only on
 * the SECOND uncleared timeout, the driver programs each single-timeout period
 * near `seconds`/2 so two expiries land close to the requested backstop; see
 * HWWATCHDOG.md "Two-timeout behaviour".  A no-op if !present().  `seconds` is
 * clamped to the chip's representable range. */
void uno_hw_wdt_arm(unsigned seconds);

/* Reload (kick) the countdown to the full period armed above.  Cheap enough to
 * call on every guard pet.  A no-op if not armed / !present(). */
void uno_hw_wdt_pet(void);

/* Halt the countdown (sets TCO_TMR_HLT).  A no-op if not armed / !present(). */
void uno_hw_wdt_disarm(void);

/* Introspection for the harness / device manager (never resets anything):
 * writes a short NUL-terminated status line (generation, TCOBASE, NO_REBOOT
 * state, armed period, current reload value) and returns its length. */
int  uno_hw_wdt_status(char *buf, int cap);

/* One-line command dispatch for the `uno.hwwdt(line)` pc64-python binding and
 * ad-hoc operator use over URC.  Writes a short NUL-terminated reply and returns
 * its length; -1 on an unknown command.  Subcommands:
 *   status | present            introspection (the default)
 *   arm <seconds>               arm the hardware countdown
 *   pet                         reload it
 *   disarm                      halt it
 *   selftest <seconds>          arm, then `cli; for(;;){}` - the IRQs-off wedge
 *                               that DEFEATS the software guard; only the TCO can
 *                               reset from here.  NEVER RETURNS.  This is the
 *                               metal validation trigger (and the QEMU smoke).
 *   wedge                       `cli; for(;;){}` with NO arm - for confirming
 *                               the software guard alone canNOT recover it.
 * The two spinning forms are the deliberate, self-contained way to exercise the
 * one wedge class the software guard misses, without editing the harness. */
int  uno_hw_wdt_cmd(const char *line, char *out, int cap);

/* Opt-in boot self-demonstration (STRESS.CFG key `hw-wdt-selftest[=<seconds>]`):
 * arm the TCO then cli-spin, so the hardware backstop resets the box with no
 * unoautomate wiring in place.  Wired as one guarded call in uefi_main.c's debug
 * boot block; a no-op without the key / without a usable TCO. */
void uno_hw_wdt_boot_selftest(void);

#else /* !UNO_DEBUG: every hook compiles away, prod image is untouched */
#define uno_hw_wdt_present()   0
#define uno_hw_wdt_arm(s)      ((void)0)
#define uno_hw_wdt_pet()       ((void)0)
#define uno_hw_wdt_disarm()    ((void)0)
#define uno_hw_wdt_status(b,c) 0
#define uno_hw_wdt_cmd(l,b,c)  (-1)
#define uno_hw_wdt_boot_selftest() ((void)0)
#endif

#endif /* UNO_HW_WDT_H */

/* ===========================================================================
 * UnoDOS/pc64 DEBUG BUILD - the metal stress driver.
 *
 * Called once per main-loop frame (pc64_stress_tick, wired in pc64_uui.c's
 * main loop under -DUNO_DEBUG).  It drives the OS through the failure classes
 * the deep review catalogued, using the SHELL'S OWN machinery (open_app,
 * synthetic input through map_key, the native FAT layer) so a run exercises
 * the true stack, not a private shadow of it.  Every crash it provokes is
 * captured by uno_debug.c and lands in CRASH\ on the boot volume.
 *
 * OFF by default: it does nothing unless STRESS.CFG exists on a volume.  A
 * normal flashed stick therefore boots to an ordinary desktop; a tester drops
 * STRESS.CFG (any contents; keys documented in DEBUG.md) to arm it.
 *
 * Boot-loop safe: if CRASH\ already holds many reports, the driver throttles
 * to the gentler phases so a reproducible early crash doesn't loop the machine
 * forever writing the same report.  "mode=once" stops after a single full pass.
 * ======================================================================== */
#ifdef UNO_DEBUG

#include "fb.h"
#include "pc64_fs.h"
#include "fat.h"
#include "uno_debug.h"
#include <string.h>

int  snprintf(char *buf, unsigned long n, const char *fmt, ...);

/* shell hooks (pc64_uui.c, UNO_DEBUG only) */
int  pc64_dbg_app_count(void);
int  pc64_dbg_app_hidden(int a);
const char *pc64_dbg_app_name(int a);
int  pc64_dbg_app_is_open(int a);
int  pc64_dbg_open_count(void);
void pc64_dbg_open_app(int a);
void pc64_dbg_close_focused(void);
void pc64_dbg_focus_next(void);
void pc64_dbg_mark_dirty(void);
void pc64_dbg_open_path(const char *path);

/* platform hooks (uefi_main.c / uno_debug.c, UNO_DEBUG only) */
void uno_pc64_inject_key(int scan, int uni, int ctrl);
void uno_pc64_inject_pointer(int x, int y, int btn);
void uno_dbg_write_bootenv(void);
void uno_dbg_write_perf(const char *text, int len);
void uno_dbg_force(int what);

/* ---- config -------------------------------------------------------------- */
static int   g_armed = -1;              /* -1 unknown, 0 off, 1 on            */
static int   g_max_passes;              /* stop after N passes (0 = unlimited) */
static int   g_allow_force;             /* explicit opt-in to self-crash tests */
static int   g_force_kind;              /* pass-1 self-test: 0 #PF, 4 hang     */
static int   g_speed = 4;               /* frames between actions (>=1)       */
static unsigned long g_frame;
static unsigned long g_action;
static int   g_pass;

/* Match `key` as a whole word on a NON-COMMENT line.  A naive strstr over the
 * whole file is wrong: the shipped STRESS.CFG documents the keys in a comment
 * ("# keys: once fast slow allow-force"), and substring-matching that comment
 * silently enabled every key - including allow-force, so a "safe" stick forced
 * a #PF on pass 1.  Found on the Surface (CR003).  So: skip any line whose
 * first non-space char is '#', and require the key to be delimited (not a
 * substring of a longer token). */
static int tok_delim(char c) { return c == 0 || c == ' ' || c == '\t' ||
                                      c == '\r' || c == '\n' || c == '='; }
/* Returns a pointer just past `key` where it appears as a whole token on a
 * non-comment line, else NULL.  cfg_has/cfg_int are both built on this. */
static const char *cfg_find(const char *cfg, const char *key)
{
    const char *p = cfg;
    int klen = (int)strlen(key);
    while (*p) {
        const char *ls = p;                    /* line start */
        while (*ls == ' ' || *ls == '\t') ls++;
        if (*ls != '#') {                      /* not a comment line */
            const char *s = ls;
            while (*s && *s != '\n') {
                int atstart = (s == ls) || tok_delim(s[-1]);
                if (atstart && !strncmp(s, key, (size_t)klen) && tok_delim(s[klen]))
                    return s + klen;
                s++;
            }
        }
        while (*p && *p != '\n') p++;           /* advance to next line */
        if (*p) p++;
    }
    return 0;
}
static int cfg_has(const char *cfg, const char *key)
{ return cfg_find(cfg, key) != 0; }

/* `key=N` -> N, else `dflt`.  Used for passes=3. */
static int cfg_int(const char *cfg, const char *key, int dflt)
{
    const char *s = cfg_find(cfg, key);
    int v = 0, any = 0;
    if (!s) return dflt;
    while (*s == ' ' || *s == '\t' || *s == '=') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    return any ? v : dflt;
}

static void arm(void)
{
    unsigned char cfg[512];
    int v, n = uno_fs_volumes();
    long got = -1;
    g_armed = 0;
    for (v = 0; v < n && got < 0; v++)
        got = uno_fs_read(v, "STRESS.CFG", cfg, (long)sizeof cfg - 1);
    if (got < 0) { uno_dbg_log("stress: no STRESS.CFG - driver disabled"); return; }
    cfg[got] = 0;
    g_armed = 1;
    /* how long to run: `passes=N` (bounded), `once` = passes=1, neither =
     * forever.  A BOUNDED run matters on metal: while the driver is arming
     * and opening apps every few frames the operator cannot reach the Start
     * menu to shut down cleanly, so an unbounded run can only be ended by
     * pulling the power. */
    g_max_passes = cfg_has((char *)cfg, "once") ? 1 : 0;
    g_max_passes = cfg_int((char *)cfg, "passes", g_max_passes);
    /* self-test on pass 1 (proves the crash/hang pipeline end to end on metal):
     *   allow-force -> a #PF (crash report path)
     *   force-hang  -> an infinite loop (watchdog / HG report path)
     * Either implies "force"; both off = the driver never self-triggers. */
    g_allow_force = cfg_has((char *)cfg, "allow-force") || cfg_has((char *)cfg, "force-hang");
    g_force_kind  = cfg_has((char *)cfg, "force-hang") ? 4 : 0;
    if (cfg_has((char *)cfg, "slow"))  g_speed = 12;
    if (cfg_has((char *)cfg, "fast"))  g_speed = 1;
    {   /* boot-loop guard: throttle hard once several reports have piled up */
        int cr = uno_dbg_crash_count();
        if (cr >= 8) { g_speed = 20; g_allow_force = 0;
                       uno_dbg_log("stress: %d prior reports - THROTTLED", cr); }
    }
    uno_dbg_write_bootenv();            /* snapshot BOOTENV.TXT while safe */
    uno_dbg_log("stress: ARMED (passes=%d force=%d/kind%d speed=%d, %d prior reports)",
                g_max_passes, g_allow_force, g_force_kind, g_speed,
                uno_dbg_crash_count());
}

/* ---- a tiny deterministic PRNG (no Math.random equivalent needed) -------- */
static unsigned long g_rng = 0x2545F491u;
static unsigned rnd(unsigned m) { g_rng = g_rng * 1103515245u + 12345u; return m ? (g_rng >> 8) % m : 0; }

/* ===========================================================================
 * phases - one small action per call, indexed by g_action so the driver
 * never blocks the frame.  Order is chosen so the cheap smoke tests run
 * first each pass and the aggressive ones last.
 * ======================================================================== */

/* A: open every visible app once, then close it - the smoke matrix.  Reaches
 *    each app's init + first render (the null-init / partial-OOM classes). */
static int g_smoke_app;
static void phase_smoke(void)
{
    int napps = pc64_dbg_app_count();
    while (g_smoke_app < napps && pc64_dbg_app_hidden(g_smoke_app)) g_smoke_app++;
    if (g_smoke_app >= napps) { g_smoke_app = 0; return; }
    uno_dbg_check("stress:smoke");
    uno_dbg_log("stress: open %s", pc64_dbg_app_name(g_smoke_app));
    pc64_dbg_open_app(g_smoke_app);
    g_smoke_app++;
}

/* B: pointer storms.  Deliberately includes the huge-delta fling that
 *    overflowed int32 on a wide desktop (delta x width x speed > INT_MAX)
 *    - the X1 cursor bug - plus normal sweeps and edge slams. */
static void phase_pointer(void)
{
    int w = FB_W, h = FB_H, k = (int)(g_action & 7);
    uno_dbg_check("stress:pointer");
    switch (k) {
    case 0: uno_pc64_inject_pointer(0, 0, 0); break;
    case 1: uno_pc64_inject_pointer(w - 1, h - 1, 1); break;      /* corner + click */
    case 2: uno_pc64_inject_pointer(w - 1, h - 1, 0); break;      /* release */
    case 3: uno_pc64_inject_pointer((int)rnd((unsigned)w), (int)rnd((unsigned)h), 0); break;
    case 4: uno_pc64_inject_pointer(w / 2, 0, 0); break;          /* taskbar edge */
    case 5: uno_pc64_inject_pointer(w / 2, h / 2, 1); break;      /* centre click */
    case 6: uno_pc64_inject_pointer(w / 2, h / 2, 0); break;
    case 7: uno_pc64_inject_pointer((int)rnd((unsigned)w), 4, 0); break;
    }
}

/* C: keyboard storms into whatever is focused - typing, arrows, ctrl-combos,
 *    and Ctrl-W (the shell's close) so windows churn.  Reaches editor/Studio
 *    buffers, the caret/wrap TextWidth path, list navigation. */
static void phase_keys(void)
{
    static const char *bursts[] = {
        "the quick brown fox jumps over 0123456789 ",
        "\x08\x08\x08\x08 backspace storm ",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "{}[]()<>;:'\"\\|/?.,~`!@#$%^&*-_=+ ",
    };
    int k = (int)(g_action & 3), i;
    const char *s = bursts[k];
    uno_dbg_check("stress:keys");
    for (i = 0; s[i]; i++) uno_pc64_inject_key(0, (unsigned char)s[i], 0);
    /* arrow navigation (List/editor) + Enter (load selection) */
    uno_pc64_inject_key(0x7D, 0x1F, 0);        /* Down  */
    uno_pc64_inject_key(0x7D, 0x1F, 0);
    uno_pc64_inject_key(0x24, 0x0D, 0);        /* Enter */
}

/* D: a deep directory tree with long names on the boot volume, then list it -
 *    the fixed-stack path-buffer overflow class (Files head[64] / Photos
 *    np[120]).  Bounded depth so it can't fill the disk. */
static int g_tree_built;
static void phase_deeptree(void)
{
    int vol = -1, v, n = uno_fat_volumes();
    char path[256];
    int depth, pl;
    if (g_tree_built) return;
    for (v = 0; v < n; v++) if (uno_fs_writable(v)) { vol = v; break; }
    if (vol < 0) { g_tree_built = 1; return; }
    uno_dbg_check("stress:deeptree");
    /* STRESS\D0\D1... each level an 8.3-max name; 16 levels ~ a 150-char path */
    pl = snprintf(path, sizeof path, "STRESS");
    uno_fat_mkdir(vol, path);
    for (depth = 0; depth < 16; depth++) {
        int add = snprintf(path + pl, sizeof path - (unsigned)pl, "\\DEEPDIR%d", depth % 10);
        if (pl + add >= (int)sizeof path - 16) break;
        pl += add;
        if (!uno_fat_mkdir(vol, path)) break;
    }
    /* a long-named file at the bottom, then open the Files app onto it */
    snprintf(path + pl, sizeof path - (unsigned)pl, "\\LONGNAME.TXT");
    uno_fat_write(vol, path, (const unsigned char *)"deep\n", 5);
    uno_dbg_log("stress: built deep tree (%d levels), path len %d",
                depth, (int)strlen(path));
    g_tree_built = 1;
}

/* E: window-cap churn - open apps faster than they close to slam the unoui
 *    window table (the silent 8-window cap / open_app rollback path). */
static void phase_wincap(void)
{
    int napps = pc64_dbg_app_count(), a = (int)(g_action % (unsigned)napps);
    uno_dbg_check("stress:wincap");
    if (!pc64_dbg_app_hidden(a)) pc64_dbg_open_app(a);
    if ((g_action & 3) == 3) pc64_dbg_focus_next();
}

/* F: memory pressure via OOM injection - fail every Nth malloc for a while,
 *    then release, while apps open/close.  The partial-failure / NULL-cache
 *    class (Photos NULL map, Studio net allocs). */
static void phase_oom(void)
{
    uno_dbg_check("stress:oom");
    if ((g_action & 15) == 0) {
        uno_dbg_oom_every = 32 + (int)rnd(64);
        uno_dbg_log("stress: OOM injection every %d mallocs", uno_dbg_oom_every);
    } else if ((g_action & 15) == 12) {
        uno_dbg_oom_every = 0;                 /* release before smoke reopens */
    }
    { int napps = pc64_dbg_app_count(), a = (int)rnd((unsigned)napps);
      if (!pc64_dbg_app_hidden(a)) pc64_dbg_open_app(a); }
}

/* G: malformed-input corpus - open the crafted files the build staged into
 *    STRESS\CORPUS\ through the shell's document path (text/markup parsers).
 *    The image/audio corpus lives in PICTURES\ and MEDIA\ and is reached by
 *    the smoke phase opening Photos/Music onto it. */
static const char *kCorpus[] = {
    "STRESS\\CORPUS\\TRUNC.HTM", "STRESS\\CORPUS\\HUGE.HTM",
    "STRESS\\CORPUS\\NUL.MD",    "STRESS\\CORPUS\\DEEP.HTM",
    "STRESS\\CORPUS\\BADUTF.TXT"
};
static void phase_corpus(void)
{
    int k = (int)(g_action % (unsigned)(sizeof kCorpus / sizeof kCorpus[0]));
    uno_dbg_check("stress:corpus");
    uno_dbg_log("stress: open corpus %s", kCorpus[k]);
    pc64_dbg_open_path(kCorpus[k]);
}

/* H: close whatever is focused, keeping the window count from growing without
 *    bound across passes. */
static void phase_close(void)
{
    uno_dbg_check("stress:close");
    if (pc64_dbg_open_count() > 3) pc64_dbg_close_focused();
    pc64_dbg_focus_next();
}

/* the pass schedule: a weighted round-robin.  Force-crash self-tests run only
 * with allow-force in the config and only once, near the end of pass 0, so an
 * armed-but-not-force stick never self-crashes. */
static void run_action(void)
{
    static void (*const phases[])(void) = {
        phase_smoke, phase_pointer, phase_keys, phase_wincap,
        phase_deeptree, phase_oom, phase_corpus, phase_close
    };
    int nph = (int)(sizeof phases / sizeof phases[0]);
    phases[g_action % (unsigned)nph]();
    g_action++;
    pc64_dbg_mark_dirty();

    /* one full pass = touching every phase a few times */
    if (g_action % (unsigned)(nph * 6) == 0) {
        char buf[512];
        int n = snprintf(buf, sizeof buf,
            "pass %d complete  uptime=%llums  open_windows=%d  crash_reports=%d\n"
            "(see the HUD for render/present/idle; this file is a periodic\n"
            " liveness snapshot proving the driver is still running)\n",
            g_pass, uno_dbg_uptime_ms(), pc64_dbg_open_count(),
            uno_dbg_crash_count());
        uno_dbg_write_perf(buf, n);
        g_pass++;
        uno_dbg_log("stress: === pass %d ===", g_pass);

        if (g_pass == 1 && g_allow_force) {
            /* prove the whole pipeline end-to-end on real hardware:
             *   kind 0 -> a #PF: the crash-report path (CRASH\CR###, reset).
             *   kind 4 -> an infinite loop: the WATCHDOG path (heartbeat stops
             *             -> HG### + reset), so a real freeze is provably
             *             caught on this machine, not just a fault.
             * Only with explicit allow-force / force-hang in STRESS.CFG. */
            uno_dbg_log("stress: force set - triggering self-test kind %d (%s)",
                        g_force_kind, g_force_kind == 4 ? "hang/watchdog" : "#PF");
            uno_dbg_force(g_force_kind);
        }
        if (g_max_passes && g_pass >= g_max_passes) {
            /* Done: go idle and hand the machine back, so the operator can
             * reach the Start menu and Shut Down cleanly (a clean shutdown
             * also marks the boot clean + flushes).  Close what we opened so
             * they get a usable desktop rather than a pile of windows. */
            int guard = 32;
            g_armed = 0;
            while (pc64_dbg_open_count() > 1 && guard-- > 0)
                pc64_dbg_close_focused();
            pc64_dbg_mark_dirty();
            uno_dbg_log("stress: %d pass(es) done - driver IDLE, desktop is yours "
                        "(use Start > Shut Down)", g_pass);
        }
    }
}

/* Operator escape hatch (F12 in the shell).  While the driver is arming and
 * opening apps every few frames there is no way to reach the Start menu, so an
 * unbounded run could only be ended by pulling the power - which is also the
 * one exit that can lose unsynced telemetry.  This stops the driver dead and
 * clears the windows it opened, handing back a usable desktop. */
void pc64_stress_stop(void)
{
    int guard = 32;
    if (g_armed <= 0) return;
    g_armed = 0;
    uno_dbg_oom_every = 0;                 /* release any OOM injection */
    while (pc64_dbg_open_count() > 1 && guard-- > 0) pc64_dbg_close_focused();
    pc64_dbg_mark_dirty();
    uno_dbg_log("stress: STOPPED by operator (F12) after %d pass(es)", g_pass);
}

void pc64_stress_tick(void)
{
    if (g_armed < 0) {
        /* first few hundred frames: let the desktop settle, then arm once the
         * FS is definitely mounted */
        if (++g_frame < 120) return;
        arm();
    }
    if (g_armed <= 0) return;
    if ((++g_frame % (unsigned)g_speed) != 0) return;
    run_action();
}

#endif /* UNO_DEBUG */

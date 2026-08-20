/* ===========================================================================
 * UnoDOS/pc64 - UNOAUTOMATE PROBE (Stage 2): the one enumeration surface for
 * "what is the system doing right now".  See unoauto.h for the row schema.
 *
 * Everything here reads state other subsystems already keep - the F11 draw
 * profiler, the shell's window list, the net stack's counters, the walkable
 * debug heap - through their existing (or purpose-added, additive) accessors.
 * Snapshot semantics: one pass, caller memory, nothing retained.
 * ======================================================================== */

#include "unoauto.h"
#include "net.h"
#include "pc64_fs.h"

/* modules (pc64_modload.c) */
int         uno_mod_count(void);
const char *uno_mod_file(int proc);
int         uno_mod_present(const char *file);
/* shell window list (pc64_uui.c, debug-only accessors) */
int         pc64_shell_win_count(void);
const char *pc64_shell_win_title(int i);
int         pc64_shell_win_focused(int i);
/* draw profiler + uptime (uno_debug.c) */
int  uno_dbg_win_stat(int i, const char **title, unsigned long long *cyc,
                      unsigned long *max_us);
unsigned long long uno_dbg_uptime_ms(void);
unsigned long      uno_dbg_frames(void);
unsigned long      uno_dbg_win_calls(int i);
unsigned long long uno_dbg_tsc_per_ms(void);
/* walkable debug heap (pc64_libc.c) */
void uno_heap_stats(unsigned long *used, unsigned long *free_,
                    unsigned long *largest);

static int put(UnoAutoProbeEnt *out, int max, int n, const char *name,
               int kind, int state, unsigned long long v1, unsigned long long v2)
{
    if (n >= max) return n;
    out[n].name = name; out[n].kind = kind; out[n].state = state;
    out[n].v1 = v1; out[n].v2 = v2;
    return n + 1;
}

int unoauto_probe(UnoAutoProbeEnt *out, int max)
{
    int n = 0, i, cnt;

    /* subsystems first: always present, so a tiny buffer still sees them */
    { unsigned long used = 0, fre = 0, largest = 0;
      uno_heap_stats(&used, &fre, &largest);
      n = put(out, max, n, "heap", 2, 0, used, fre); }
    n = put(out, max, n, "net", 2,
            (net_link() ? 1 : 0) | (net_dhcp_done() ? 2 : 0),
            net_tx_frames(), net_rx_frames());
    { int vols = uno_fs_volumes(), w = 0;
      for (i = 0; i < vols; i++) if (uno_fs_writable(i)) w++;
      n = put(out, max, n, "fs", 2, vols, (unsigned long long)w, 0); }
    n = put(out, max, n, "shell", 2, pc64_shell_win_count(),
            uno_dbg_uptime_ms(), 0);
    /* frame count + TSC scale: the two constants that turn the per-window
     * cycle totals below into ms/frame on the host side. */
    n = put(out, max, n, "perf", 2, 0, uno_dbg_frames(), uno_dbg_tsc_per_ms());

    /* open windows, with their lifetime draw cost from the F11 profiler */
    cnt = pc64_shell_win_count();
    for (i = 0; i < cnt; i++) {
        const char *t = pc64_shell_win_title(i);
        unsigned long long cyc = 0;
        unsigned long mx = 0;
        const char *pt;
        unsigned long long pc;
        unsigned long pm;
        int j;
        for (j = 0; uno_dbg_win_stat(j, &pt, &pc, &pm); j++)
            if (pt == t) { cyc = pc; mx = pm; break; }   /* titles are literals */
        n = put(out, max, n, t ? t : "(untitled)", 1,
                pc64_shell_win_focused(i), cyc, mx);
    }

    /* kind 3: the raw per-window draw profile, one row per profiled title,
     * whether or not that window is still open - so an arm can be measured
     * after it has been closed.  state carries the CALL count, v1 the cycles,
     * v2 the worst single draw.  Emitted before the module roster because the
     * roster is long and put() silently drops rows past the caller's buffer. */
    for (i = 0; ; i++) {
        const char *pt; unsigned long long pc; unsigned long pm;
        if (!uno_dbg_win_stat(i, &pt, &pc, &pm)) break;
        n = put(out, max, n, pt ? pt : "(untitled)", 3,
                (int)uno_dbg_win_calls(i), pc, pm);
    }

    /* the .UNO module roster: what is shipped vs actually on a volume */
    cnt = uno_mod_count();
    for (i = 0; i < cnt; i++) {
        const char *f = uno_mod_file(i);
        if (f) n = put(out, max, n, f, 0, uno_mod_present(f), 0, 0);
    }
    return n;
}


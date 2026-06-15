#!/usr/bin/env python3
"""Refactor a UnoDOS core (ps2 / dreamcast / mac unodos.c) into an APP-FREE
kernel that dispatches apps through the pointer-based loader (app_loader.c).

Removes every app function body + the compile-time switch dispatch; keeps WM,
renderer, FAT12, the audio synth primitives (music_open_chan/note_on/quiet) and
the game-music engine (gm_*), input.  Anchored to TEXT so it runs identically on
ps2/dreamcast (identical app code) and mac.

Usage:  python tools/refactor_core.py  IN.c  OUT.c
"""
import sys

def idx(L, needle, after=0, required=True):
    for k in range(after, len(L)):
        if needle in L[k]:
            return k
    if required:
        raise SystemExit("not found: %r (after %d)" % (needle, after))
    return -1

_BC = [False]   # in-block-comment state carried across lines

def _braces(line):
    """Return (opens, closes) counting only real code braces (ignore comments,
    // line comments, and string/char literals)."""
    s = line; out_o = 0; out_c = 0; j = 0
    while j < len(s):
        if _BC[0]:
            k = s.find('*/', j)
            if k < 0:
                return out_o, out_c
            _BC[0] = False; j = k + 2; continue
        c = s[j]
        if c == '/' and j+1 < len(s) and s[j+1] == '*':
            _BC[0] = True; j += 2; continue
        if c == '/' and j+1 < len(s) and s[j+1] == '/':
            break
        if c == '"' or c == "'":
            q = c; j += 1
            while j < len(s):
                if s[j] == '\\': j += 2; continue
                if s[j] == q: j += 1; break
                j += 1
            continue
        if c == '{': out_o += 1
        elif c == '}': out_c += 1
        j += 1
    return out_o, out_c

def brace_end(L, i):
    """Index of the closing brace that balances the first '{' at/after line i.
    Comment/string aware (so braces inside literals/comments are ignored)."""
    _BC[0] = False
    depth = 0; seen = False; k = i
    while k < len(L):
        o, c = _braces(L[k])
        depth += o - c
        if o > 0:
            seen = True
        if seen and depth == 0:
            return k
        k += 1
    raise SystemExit("unbalanced from line %d" % i)

def del_fn(L, sig, lead_comment=None, guard=None):
    """Delete a function whose definition line contains `sig`. If lead_comment
    text is given and appears on the line just above, delete that too.  If the
    line above sig is `guard` (e.g. '#if UNO_COLOR') and the line after the
    closing brace is '#endif...', delete those wrappers too."""
    i = idx(L, sig)
    j = brace_end(L, i)
    lo, hi = i, j
    if guard and L[lo-1].strip().startswith(guard):
        lo -= 1
        if hi+1 < len(L) and L[hi+1].strip().startswith('#endif'):
            hi += 1
    if lead_comment and lead_comment in L[lo-1]:
        lo -= 1
    return L[:lo] + L[hi+1:]

def del_proto(L, sig):
    i = idx(L, sig)
    return L[:i] + L[i+1:]

def del_data(L, start_sig, end_sig=None):
    """Delete from the line containing start_sig through end_sig (or the same
    line if it ends with ';')."""
    i = idx(L, start_sig)
    if end_sig is None:
        return L[:i] + L[i+1:]
    j = idx(L, end_sig, after=i)
    return L[:i] + L[j+1:]

def del_range_comment_to_fn(L, comment_sig, last_fn_sig):
    """Delete a whole app SECTION: from the banner comment line containing
    comment_sig down to the end of the function whose def contains last_fn_sig."""
    ci = idx(L, comment_sig)
    # walk back to the start of the /* banner */
    s = ci
    while s > 0 and '/*' not in L[s]:
        s -= 1
    fi = idx(L, last_fn_sig, after=ci)
    fe = brace_end(L, fi)
    return L[:s] + L[fe+1:]

HOST_DRIVER = r'''#ifdef UNO_HOST
    /* Host driver for the REAL refactored core: launch apps as MODULES loaded
       from storage (apps_store/appNN.so via uno_load_module), drive each through
       the AppInterface pointers, settle a few frames, present a PPM and exit.
       UNO_APP=<id> renders a single app window; unset launches a desktop stack.
       This is the genuine proof: NO app code in this binary - every window's
       draw/key/tick comes from a dlopen'd module. */
    {
        const char *one = getenv("UNO_APP");
        int _i;
        if (one) {
            short proc = (short)atoi(one);
            const AppInterface *ai;
            launch_app(proc);                 /* loads module, opens a window */
            ai = app_iface(proc);
            if (ai && ai->key) {              /* nudge games into a played state */
                if (proc == APP_DOSTRIS) { ai->key('n',0,0);
                    for (_i=0;_i<8;_i++){ ai->key(0,0x7B,0); ai->key(' ',0,0); } }
                else if (proc == APP_PACMAN) { ai->key('n',0,0);
                    for (_i=0;_i<160;_i++) if (ai->tick) ai->tick(); }
                else if (proc == APP_OUTLAST) { ai->key('n',0,0);
                    for (_i=0;_i<90;_i++) if (ai->tick) ai->tick(); }
                else if (proc == APP_TRACKER) { ai->key('d',0,0);
                    for (_i=0;_i<5;_i++) ai->key(0,0x7D,0); ai->key(' ',0,0); }
                else if (proc == APP_THEME) { ai->key(0,0x7D,0); ai->key(0,0x7D,0);
                    ai->key(0,0x7D,0); ai->key('\r',0,0); }
                else if (proc == APP_MUSIC) { ai->key(' ',0,0); }
            }
        } else {
            short order[] = { APP_SYSINFO, APP_CLOCK, APP_FILES, APP_NOTEPAD,
                              APP_PACMAN, APP_THEME };
            short n, k;
            for (_i=0; _i<(int)(sizeof order/sizeof order[0]); _i++)
                launch_app(order[_i]);
            n = gZCount;
            for (k=0; k<n; k++) {             /* tile so several are visible */
                UnoWin *w = &gWins[gZ[k]];
                short ww = w->bounds.right-w->bounds.left;
                short wh = w->bounds.bottom-w->bounds.top;
                short nx = 8 + (k%3)*208, ny = MENUBAR_H+8 + (k/3)*180;
                SetRect(&w->bounds, nx, ny, nx+ww, ny+wh);
            }
        }
        for (_i = 0; _i < 16; _i++) {
            gm_tick(); tick_all_apps();
            post_ticks(); task_yield(); app_secondly();
        }
        repaint_all();
        uno_host_present();
        fprintf(stderr, "real core: %d module(s) loaded + dispatched (zero app code in core)\n", gZCount);
        return 0;
    }
#endif'''


def main():
    src = open(sys.argv[1], encoding='utf-8', errors='replace').read()
    L = src.split('\n')

    # ===== 0. Shared ABI header + remove the core's now-duplicate types =====
    # The core must see KernelApi/AppInterface/UnoWin/Note/Song/GameRGB/APP_*
    # from the shared ABI header so app_loader.c (#included later) and the
    # modules agree byte-for-byte.  uno_app.h pulls these in; we strip the
    # core's own copies to avoid redefinition.
    i = idx(L, '#include "mac_compat.h"', required=False)
    if i < 0:
        # mac build: ABI header goes after the last Toolbox include (Memory.h)
        i = idx(L, '#include <Memory.h>')
    L = L[:i+1] + [
        '#include "uno_app.h"   /* shared app ABI: KernelApi/AppInterface/UnoWin/... */',
        '#if defined(UNO_HOST)',
        '#include <stdio.h>',
        '#include <stdlib.h>',
        '#endif',
    ] + L[i+1:]

    # remove core's enum { APP_SYSINFO .. APP_THEME };  (now from uno_app.h)
    i = idx(L, 'enum { APP_SYSINFO = 0, APP_CLOCK')
    j = next(k for k in range(i, len(L)) if 'APP_THEME' in L[k])
    L = L[:i] + L[j+1:]

    # remove core's UnoWin typedef (identical to uno_app.h's)
    i = idx(L, 'typedef struct {')
    # confirm this is the UnoWin one (followed within a few lines by '} UnoWin;')
    je = next(k for k in range(i, i+8) if '} UnoWin;' in L[k])
    if 'used' in ''.join(L[i:je+1]) and 'proc' in ''.join(L[i:je+1]):
        L = L[:i] + L[je+1:]

    # remove core's Note + Song typedefs (now from uno_app.h)
    L = del_data(L, 'typedef struct { unsigned char midi; unsigned char dur; } Note;')
    L = del_data(L, 'typedef struct { const Note *notes; short count; const char *title; } Song;')

    # remove core's GameRGB typedef (now from uno_app.h)
    L = del_data(L, 'typedef struct { unsigned char r, g, b, mono; } GameRGB;')

    # Theme palette tables + theme-app state move to the Theme module.  KEEP
    # kPalette/kBlack (KernelApi + renderer).  Remove NTHEMES/kThemeNames/
    # kThemes/gTSel/gTSlot from the core.
    i = idx(L, '#define NTHEMES 8')
    # the preceding 3-line comment about preset palettes goes too
    if 'preset palettes shared' in L[i-1] or 'preset palettes shared' in L[i-3]:
        s = i
        while s > 0 and '/*' not in L[s]:
            s -= 1
        i = s
    j = idx(L, 'static short gTSel = 0, gTSlot = 0;', after=i)
    L = L[:i] + L[j+1:]

    # ===== 1. App forward declarations (theme..pacman) ======================
    # Replace the per-app prototypes with forward decls for the loader's
    # pointer-based dispatch (app_loader.c is #included later, after the kernel
    # helpers it needs are defined; app_tick_dispatch above the include needs
    # these forward decls).
    i = idx(L, 'static void theme_draw(UnoWin *w);')
    if '#if UNO_COLOR' in L[i-1]:
        i -= 1
    j = idx(L, 'static void pacman_tick(void);', after=i)
    loader_protos = [
        "/* pointer-based dispatch supplied by app_loader.c (no app code here) */",
        "static const AppInterface *app_iface(short proc);",
        "static const char *app_title(short proc);",
        "static void app_default_rect(short proc, Rect *r);",
        "/* fill_rgb + gm_start are defined after the #included app_loader.c;",
        "   forward-declare them so the loader's KernelApi build compiles. */",
        "static void fill_rgb(Rect *q, const GameRGB *c);",
        "static void gm_start(const Note *notes, short count, short owner);",
    ]
    L = L[:i] + loader_protos + L[j+1:]

    # ===== 2. kWinTitles + kWinRect (now from modules) ======================
    L = del_data(L, 'static const char *kWinTitles[NAPPS]')
    i = idx(L, 'static const short kWinRect[NAPPS][4] = {')
    if 'default window bounds per app' in L[i-1]:
        i -= 1
    j = next(k for k in range(i, len(L)) if L[k].strip() == '};')
    L = L[:i] + L[j+1:]

    # ===== 2b. launch_app: title + default bounds come from the loaded module
    i = idx(L, 'gWins[slot].title = kWinTitles[proc];')
    # the SetRect(...kWinRect...) spans two lines
    L = (L[:i]
         + ['    gWins[slot].title = app_title(proc);',
            '    app_default_rect(proc, &gWins[slot].bounds);']
         + L[i+3:])

    # ===== 3. Files app section (banner .. files_click) =====================
    L = del_range_comment_to_fn(L, 'Files app', 'static void files_click(UnoWin *w, Point p)')

    # ===== 4. Notepad app section (banner .. notepad_key) ===================
    L = del_range_comment_to_fn(L, 'Notepad app', 'static Boolean notepad_key(char ch, short code, Boolean cmd)')

    # ===== 5. Music app: keep synth primitives, drop song tables + UI =======
    # Drop the song note tables (kCanon..NSONGS) + Music-app state, but KEEP
    # gSnd (the synth channel) and the music_open_chan/note_on/quiet primitives.
    # The QN/EN/HN/DQ tempo defines belong to the song tables -> remove them too.
    i = idx(L, '#define QN 30')
    j = idx(L, '#define NSONGS', after=i)
    L = L[:i] + L[j+1:]
    # remove the Music-app sequencer state + CURNOTES/CURCOUNT (keep gSnd)
    for s in ['static Boolean gPlaying = false;',
              'static short   gNoteIx = 0;',
              'static long    gNoteEnd = 0;',
              'static short   gSong = 0;',
              '#define CURNOTES (kSongs[gSong].notes)',
              '#define CURCOUNT (kSongs[gSong].count)']:
        L = del_data(L, s)
    # Drop Music UI/sequencer fns: music_draw, music_tick, music_select,
    # music_key (these move into the Music module).  KEEP music_open_chan/
    # note_on/quiet (synth primitives in the KernelApi).  music_start/music_stop
    # stay in the KernelApi but lose their song-state body -> channel-level ops.
    L = del_fn(L, 'static void music_draw(UnoWin *w)')
    L = del_fn(L, 'static void music_tick(void)')
    L = del_fn(L, 'static void music_select(short s)')
    L = del_fn(L, 'static Boolean music_key(char ch, short code)')
    # rebody music_start / music_stop (KernelApi exports them)
    i = idx(L, 'static void music_start(void)')
    j = brace_end(L, i)
    L = L[:i] + [
        "/* KernelApi-level music control: the Music MODULE owns the song",
        "   sequencer; these are channel-level (open / quiet) for the ABI. */",
        "static void music_start(void) { music_open_chan(); }",
    ] + L[j+1:]
    i = idx(L, 'static void music_stop(void)')
    j = brace_end(L, i)
    L = L[:i] + ["static void music_stop(void)  { music_quiet(); }"] + L[j+1:]

    # ===== 6. Tracker app section (TK_* defines .. tracker_key) =============
    L = del_range_comment_to_fn(L, '#define TK_ROWS  32', 'static Boolean tracker_key(char ch, short code)')

    # ===== 7. SysInfo + Clock section (sysinfo_draw .. clock_draw) =========
    # drop the paint forward-decl block (comment banner + 4 protos) above it.
    i = idx(L, 'Paint - MacPaint-style editor (implementation below')
    s = i
    while s > 0 and '/*' not in L[s]:
        s -= 1
    e = idx(L, 'static void paint_open(void);', after=i)
    L = L[:s] + L[e+1:]
    L = del_range_comment_to_fn(L, 'static void sysinfo_draw(UnoWin *w)', 'static void clock_draw(UnoWin *w)')

    # ===== 8. app_tick_dispatch: make it pointer-based (loader) ============
    i = idx(L, 'static void app_tick_dispatch(short proc)')
    j = brace_end(L, i)
    L = L[:i] + [
        "static void app_tick_dispatch(short proc)",
        "{",
        "    const AppInterface *ai = app_iface(proc);",
        "    if (ai && ai->tick) ai->tick();",
        "}",
    ] + L[j+1:]

    # ===== 9. switch dispatch fns: replace the whole run with #include ======
    # draw_app_content .. app_close are contiguous (with app_click/app_opened).
    i = idx(L, 'static void draw_app_content(short proc, UnoWin *w)\n') if False else idx(L, 'static void draw_app_content(short proc, UnoWin *w)')
    # ensure this is the DEFINITION (has a following '{'), not a proto
    while L[i].rstrip().endswith(';'):
        i = idx(L, 'static void draw_app_content(short proc, UnoWin *w)', after=i+1)
    # the run ends at app_close's closing brace
    ac = idx(L, 'static void app_close(short proc)', after=i)
    ace = brace_end(L, ac)
    L = L[:i] + [
        "/* App dispatch is now pointer-based: app_loader.c provides",
        "   draw_app_content / app_key / app_click / app_opened / app_close /",
        "   app_title / app_default_rect, dispatching through each module's",
        "   AppInterface.  No switch(proc) on app identity remains in the core. */",
        '#include "app_loader.c"',
        "",
        "/* Per-frame tick for every open app window, through the module's tick",
        "   pointer (replaces the old music_tick()/tracker_tick() direct calls). */",
        "static void tick_all_apps(void)",
        "{",
        "    short z;",
        "    for (z = 0; z < gZCount; z++)",
        "        app_tick_dispatch(zwin(z)->proc);",
        "}",
    ] + L[ace+1:]

    # ----- strip the per-app AUTOTEST blocks from main(): they poked the old
    #        built-in app symbols directly (now deleted).  Native autotest now
    #        launches the app as a MODULE through launch_app(). -----------------
    autotests = [
        ('#ifdef UNO_AUTOTEST_FILES',   ['    launch_app(APP_FILES);']),
        ('#if defined(UNO_AUTOTEST_THEME) && UNO_COLOR', ['    launch_app(APP_THEME);']),
        ('#ifdef UNO_AUTOTEST_DOSTRIS', ['    launch_app(APP_DOSTRIS);']),
        ('#ifdef UNO_AUTOTEST_OUTLAST', ['    launch_app(APP_OUTLAST);']),
        ('#ifdef UNO_AUTOTEST_PACMAN',  ['    launch_app(APP_PACMAN);']),
        ('#ifdef UNO_AUTOTEST_TRACKER', ['    launch_app(APP_TRACKER);']),
        ('#ifdef UNO_AUTOTEST_PAINT',   ['    launch_app(APP_PAINT);']),
        ('#ifdef UNO_AUTOTEST_FAT12',   ['    launch_app(APP_FILES);']),
        ('#ifdef UNO_AUTOTEST_MCSAVE',  ['    launch_app(APP_FILES);', '    launch_app(APP_NOTEPAD);']),
        ('#ifdef UNO_AUTOTEST_MCLOAD',  ['    launch_app(APP_FILES);', '    launch_app(APP_NOTEPAD);']),
        ('#ifdef UNO_AUTOTEST\n',       ['    launch_app(APP_MUSIC);', '    launch_app(APP_FILES);', '    launch_app(APP_NOTEPAD);']),
    ]
    for guard, body in autotests:
        gname = guard.rstrip('\n')
        exact = guard.endswith('\n')   # plain UNO_AUTOTEST must match the whole line
        i = -1
        for k in range(len(L)):
            if (L[k].strip() == gname) if exact else (gname in L[k]):
                i = k; break
        if i < 0:
            continue
        # match the balancing #endif, tracking nested #if/#ifdef/#ifndef
        depth = 0; j = None
        for k in range(i, len(L)):
            t = L[k].lstrip()
            if t.startswith('#if'):
                depth += 1
            elif t.startswith('#endif'):
                depth -= 1
                if depth == 0:
                    j = k; break
        if j is None:
            raise SystemExit("no balancing #endif for %s" % gname)
        L = L[:i] + [gname] + body + ['#endif'] + L[j+1:]

    # ----- call app_loader_init() once in main(), before any launch_app, so
    #        the KernelApi table is populated for the loaded modules. ----------
    i = idx(L, 'sched_init();')
    L = L[:i+1] + ['    app_loader_init();              /* build the KernelApi for loaded modules */'] + L[i+1:]

    # ----- replace the UNO_HOST present block with an all-apps module driver
    #        (PS2/DC/host cores have one; the native Mac core does not). --------
    i = idx(L, '#ifdef UNO_HOST', required=False)
    if i >= 0:
        j = idx(L, '#endif', after=i)
        host_block = HOST_DRIVER.split('\n')
        L = L[:i] + host_block + L[j+1:]

    # ----- rewire the event-loop's app-tick calls to the generic dispatcher
    #        (done AFTER the host block replacement so the only music_tick()/
    #        tracker_tick() left are the event loop's three separate lines). ---
    for k in range(len(L) - 2):
        if (L[k].strip() == 'music_tick();' and
                L[k+1].strip() == 'gm_tick();' and
                L[k+2].strip() == 'tracker_tick();'):
            L = L[:k] + ['        gm_tick();', '        tick_all_apps();'] + L[k+3:]
            break

    # ===== 10. Paint app section (banner .. paint_click) ===================
    L = del_range_comment_to_fn(L, 'Paint - MacPaint-style bitmap editor', 'static void paint_click(UnoWin *w, Point p)')

    # ===== 11. Dostris app section (banner .. dostris_tick) ===============
    L = del_range_comment_to_fn(L, 'Dostris - falling-blocks', 'static void dostris_tick(void)')

    # ===== 12. OutLast app section (banner .. outlast_tick) ==============
    L = del_range_comment_to_fn(L, 'OutLast - pseudo-3D', 'static void outlast_tick(void)')

    # ===== 12b. Game-music note tables: each game MODULE carries its own copy
    #            and hands the pointer to gm_start(), so the kernel's kKoro/
    #            kDrive are dead.  Remove them (keep the gm_* engine + gGm* state).
    for s in ['static const Note kKoro[] =', '#define N_KKORO',
              'static const Note kDrive[] =', '#define N_KDRIVE']:
        i = idx(L, s, required=False)
        if i >= 0:
            L = L[:i] + L[i+1:]

    # ===== 13. Pac-Man app section (banner .. pacman_tick) ==============
    L = del_range_comment_to_fn(L, 'Pac-Man - port of', 'static void pacman_tick(void)')

    # ===== 14. Theme app section (theme_draw .. theme_key, #if UNO_COLOR) ==
    i = idx(L, 'static void theme_draw(UnoWin *w)')
    # walk back over the banner + #if UNO_COLOR
    s = i
    while s > 0 and '#if UNO_COLOR' not in L[s]:
        s -= 1
    # include the banner comment above the #if
    cs = s
    while cs > 0 and '/*' not in L[cs]:
        cs -= 1
    je = idx(L, 'static Boolean theme_key(char ch, short code)', after=i)
    jee = brace_end(L, je)
    # consume trailing '#endif /* UNO_COLOR */'
    if jee+1 < len(L) and L[jee+1].strip().startswith('#endif'):
        jee += 1
    L = L[:cs] + L[jee+1:]

    out = '\n'.join(L)
    open(sys.argv[2], 'w', encoding='utf-8').write(out)
    print("refactored core ->", sys.argv[2], "(%d lines)" % len(L))

if __name__ == '__main__':
    main()

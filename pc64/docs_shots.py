#!/usr/bin/env python3
"""Capture the UnoDOS/pc64 screenshots the user manual needs.

Self-contained QMP driver (same technique as pc64/harness.py): boots the real
ESP under QEMU + OVMF headless, drives the desktop over QMP, and dumps the GOP
surface to shots/manual/<tag>.png at each scene.

  python3 docs_shots.py [scene ...]     run named scenes (default: all core)

Scenes name apps by ID - A("uocalc"), never a number. The Start-menu order is
fixed for a build but MOVES when an app is added, and a scene that counts
`down` presses does not fail when it does: it opens the next app along and
captures it under the old name. That shipped once already (2026-08-04, UnoAmp
at 7 pushed every game down one) and it is what `harness.py unoapps` had been
doing wrong for three weeks.

Two things stop it here. The order comes from build/apps_roster.txt, written by
`UNO_DEBUG=1 ./build.sh && python3 harness.py unoapps` - a run that opens every
app BY ID over URC and checks the window that appeared, so the file is a record
of a proof rather than a table somebody maintained. And before any scene runs,
count_menu_rows() asks the live production menu how many rows it has and refuses
to capture anything if that disagrees with the roster.

Why not read the roster from the machine being photographed: these figures come
from a PRODUCTION build (the debug build paints a perf HUD over every frame and
nothing toggles it off), and URC on a production build wants a token typed at
the console. The app set is a property of the build and of APPS\\, not of
UNO_DEBUG, so measuring it on the debug build of the same tree is sound - and
the row-count check is what makes that assumption say so when it is wrong.

Launch app N: Ctrl-Esc, Down*N, Enter.  Close focused window: Ctrl-W.

What none of this removes: a launch is still `Down` pressed N times, and a
dropped keystroke opens the app ABOVE the one asked for. The row count proves
the ROSTER matches the machine, not that any one walk down the menu arrived
where it meant to - it caught itself doing exactly that, twice, before the
retry below was added. A dropped key is visible in the figure (it is a picture
of the wrong app under the right name), so proof-read a regenerated set rather
than assuming it. The way to close this properly is URC on a production build,
which needs a token typed at the console.
"""
import json, os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
QMP_SOCK  = "/tmp/unodos-pc64-docs-qmp.sock"
OUTDIR    = "shots/manual"
W, H = 1280, 800                       # GOP surface (screendump size)


class Qmp:
    def __init__(self, path, timeout=40):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.3)
        self.buf = b""
        self.recv()
        self.cmd("qmp_capabilities")

    def recv(self):
        while b"\n" not in self.buf:
            self.buf += self.s.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.s.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            r = self.recv()
            if "return" in r or "error" in r:
                return r


def key(q, *names, hold=40, gap=0.12):
    for n in names:
        q.cmd("send-key", keys=[{"type": "qcode", "data": n}], **{"hold-time": hold})
        time.sleep(gap)


def combo(q, *names, gap=0.2):
    q.cmd("send-key", keys=[{"type": "qcode", "data": n} for n in names])
    time.sleep(gap)


# QEMU qcodes for the characters the scenes type. A character MISSING here used
# to be typed as its own name, which qemu silently ignores - so "=A1+A2" reached
# the guest as "A1A2" and UnoCalc stored it as text. Anything new a scene types
# has to be added here first.
QMAP = {" ": "spc", ".": "dot", ",": "comma", "-": "minus", "/": "slash",
        ":": "shift+semicolon", "_": "shift+minus",
        "=": "equal", "+": "shift+equal", "*": "shift+8",
        "(": "shift+9", ")": "shift+0", "!": "shift+1", "%": "shift+5",
        ";": "semicolon", "?": "shift+slash", "'": "apostrophe",
        "\\": "backslash", ">": "shift+dot"}


def text(q, s, gap=0.06):
    for ch in s:
        if ch.isupper():
            q.cmd("send-key", keys=[{"type": "qcode", "data": "shift"},
                                    {"type": "qcode", "data": ch.lower()}])
        elif ch in QMAP and "+" in QMAP[ch]:
            a, b = QMAP[ch].split("+")
            q.cmd("send-key", keys=[{"type": "qcode", "data": a},
                                    {"type": "qcode", "data": b}])
        elif ch.isalnum() or ch in QMAP:
            q.cmd("send-key", keys=[{"type": "qcode", "data": QMAP.get(ch, ch)}])
        else:
            raise KeyError("no qcode for %r - add it to QMAP" % ch)
        time.sleep(gap)


def mmove(q, x, y):
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / H)}}])
    time.sleep(0.1)


def mbtn(q, down):
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"down": down, "button": "left"}}])
    time.sleep(0.12)


def click(q, x, y):
    mmove(q, x, y); mbtn(q, True); mbtn(q, False); time.sleep(0.25)


def drag(q, x0, y0, x1, y1, steps=8):
    mmove(q, x0, y0); mbtn(q, True)
    for i in range(1, steps + 1):
        mmove(q, x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
    mbtn(q, False); time.sleep(0.3)


def shot(q, tag):
    os.makedirs(OUTDIR, exist_ok=True)
    ppm = "%s/%s.ppm" % (OUTDIR, tag)
    q.cmd("screendump", filename=ppm)
    time.sleep(0.4)
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm, "%s/%s.png" % (OUTDIR, tag)],
                   check=True)
    os.remove(ppm)
    print("shot: %s/%s.png" % (OUTDIR, tag), flush=True)


def wait_splash(q, timeout=30):
    """Poll screendumps until the UnoDOS splash is actually on screen (deep
    navy backdrop), so the splash shot is never the pre-GOP black frame or
    the firmware's own logo. Returns once seen (or on timeout)."""
    os.makedirs(OUTDIR, exist_ok=True)
    probe = "%s/_probe.ppm" % OUTDIR
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            q.cmd("screendump", filename=probe)
            time.sleep(0.25)
            with open(probe, "rb") as f:
                if f.readline().strip() != b"P6":
                    raise ValueError
                line = f.readline()
                while line.startswith(b"#"):
                    line = f.readline()
                w, h = map(int, line.split())
                f.readline()
                px = f.read()
            o = (10 * w + 10) * 3                     # a corner pixel
            r, g, b = px[o], px[o + 1], px[o + 2]
            if b > 34 and b > r + 12 and g < 90:      # the navy backdrop
                os.remove(probe)
                return True
        except Exception:
            pass
        time.sleep(0.5)
    if os.path.exists(probe):
        os.remove(probe)
    return False


def close_all(q):
    """Close any open windows so we return to a bare desktop."""
    for _ in range(6):
        combo(q, "ctrl", "w"); time.sleep(0.2)
    time.sleep(0.3)


def launch(q, idx, settle=1.6, gap=0.09):
    """Open Start menu and pick app `idx` by keyboard.

    `gap` is the pause between Down presses. The default is tuned for a machine
    with nothing else running; once a scene has a heavy app open (Duum renders
    every frame in Python) the guest can miss keys, and a missed key opens the
    app ABOVE the one asked for - silently, under the right name. Pass a wider
    gap when launching on top of something busy."""
    combo(q, "ctrl", "esc"); time.sleep(0.6)
    for _ in range(idx):
        key(q, "down", gap=gap)
    key(q, "ret"); time.sleep(settle)


# ------------------------------------------------------------------ the roster
# The Start-menu order, by id. Written by `harness.py unoapps` on a debug build
# of this tree: it opens every app by id over URC and checks the window that
# appeared, so this list is the residue of a proof. FALLBACK below is what the
# file said when this was written, kept so the manual can be regenerated from a
# single production build - it is checked against the live menu either way, so
# a stale fallback stops the run instead of mislabelling a figure.
ROSTER_FILE = "build/apps_roster.txt"
FALLBACK = ["control", "editor", "files", "system", "clock", "install",
            "music", "unoamp", "dostris", "pacman", "outlast", "tracker",
            "paint", "runner3d", "browser", "studio", "photos", "ssh",
            "uoword", "uocalc", "uoshow", "logview", "vmgr", "unocode", "duum"]
MENU = []                                    # ids in menu order, filled by main


def load_roster():
    """The menu order: the measured file if there is one, else FALLBACK."""
    if not os.path.exists(ROSTER_FILE):
        print("roster: %s absent, using the built-in fallback (%d apps). "
              "Measure it with: UNO_DEBUG=1 ./build.sh && python3 harness.py "
              "unoapps" % (ROSTER_FILE, len(FALLBACK)))
        return list(FALLBACK)
    ids, stamp = [], ""
    with open(ROSTER_FILE) as f:
        for line in f:
            line = line.strip()
            if line.startswith("# commit"):
                stamp = line[2:]
            if not line or line.startswith("#"):
                continue
            ids.append(line.split()[0])
    print("roster: %s, %d apps (%s)" % (ROSTER_FILE, len(ids), stamp))
    return ids


def A(app_id):
    """The menu index of an app, by id. Raises rather than guessing.

    A scene that asked for an app this build does not have used to be
    impossible to write - the constants were numbers, so a removed app silently
    became its neighbour. Now it stops the scene and names what it wanted."""
    try:
        return MENU.index(app_id)
    except ValueError:
        raise KeyError("no app %r in the menu (%s)" % (app_id, ", ".join(MENU)))


def frame_above_taskbar(path, drop=72):
    """A screendump's pixels with the taskbar cropped off.

    The taskbar clock shows SECONDS, so two full frames of an unchanged screen
    are never equal and any comparison of whole frames answers "different"
    whatever it was asked. That is not a hypothetical: it is what the row count
    below reported the first time it ran."""
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise ValueError("not a P6 ppm: " + path)
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        f.readline()                                  # maxval
        px = f.read()
    return px[:max(0, h - drop) * w * 3]


def count_menu_rows(q, expect, probe="_rows", tries=3):
    """Prove the live menu has `expect` rows, before anything is captured.

    The launcher CLAMPS at its last row (`if (i < total)` in launcher_event), so
    pressing Down far more times than there are rows always lands on the last
    one. Call that frame `end`. Then the roster is right exactly when Down
    pressed expect-1 times reaches `end` and expect-2 times does not.

    SETTLE IS LOAD-BEARING, and it cost two whole capture runs to learn. At a
    0.35 s settle this reported "the menu is longer than the roster" three times
    running on a menu that was exactly the right length: the screendump was
    racing the repaint after the last keypress, so the frame it compared was one
    the shell had not finished drawing. Measured afterwards at 1.2 s, 22 downs
    reaches the end and 21 does not, which is a 23-row menu to the row. A check
    that cries wolf is worse than no check, because the next person turns it off.

    The retries below are insurance on top of that, and they are one-sided for a
    reason: anything that goes wrong here - a dropped keystroke, a frame caught
    early - leaves you EARLIER in the list, never further along. So it can make
    expect-1 fall short of `end` (retryable, and a false alarm) but it cannot
    make expect-2 reach `end` (so one comparison settles that direction)."""
    def menu_frame(downs, tag):
        combo(q, "ctrl", "esc"); time.sleep(0.8)      # toggle_launcher resets
        for _ in range(downs):                        # scroll and hot row, so
            key(q, "down", gap=0.09)                  # each open starts at 0
        time.sleep(1.2)                               # see SETTLE above
        path = "%s/%s%s.ppm" % (OUTDIR, probe, tag)
        q.cmd("screendump", filename=path); time.sleep(0.4)
        combo(q, "ctrl", "esc"); time.sleep(0.4)
        data = frame_above_taskbar(path)
        os.remove(path)
        return data

    os.makedirs(OUTDIR, exist_ok=True)
    end = menu_frame(expect + 8, "end")               # certainly the last row
    for attempt in range(tries):
        if menu_frame(expect - 1, "at") == end:
            break
        print("menu: row %d did not reach the end, retrying (%d/%d)"
              % (expect - 1, attempt + 1, tries))
    else:
        raise SystemExit(
            "menu row count: the roster says %d apps but %d presses never "
            "reached the last row in %d tries, so the menu is longer. "
            "Re-measure it: UNO_DEBUG=1 ./build.sh && python3 harness.py "
            "unoapps" % (expect, expect - 1, tries))
    if menu_frame(expect - 2, "before") == end:
        raise SystemExit(
            "menu row count: the roster says %d apps but %d presses already "
            "reach the last row, so the menu is shorter. Re-measure it: "
            "UNO_DEBUG=1 ./build.sh && python3 harness.py unoapps"
            % (expect, expect - 2))
    print("menu: %d rows, matching the roster" % expect)

# ---- scenes ---------------------------------------------------------------
def sc_desktop(q):
    close_all(q)                     # boot opens Control Panel; close it
    shot(q, "desktop")

def sc_startmenu(q):
    close_all(q)
    combo(q, "ctrl", "esc"); time.sleep(0.8)
    shot(q, "startmenu")
    combo(q, "ctrl", "esc"); time.sleep(0.3)

# The Control Panel is a TABBED window (2026-07-26). Focus model: the first Tab
# lands on the tab STRIP; with the strip focused, Left/Right switch tabs; a
# further Tab steps into the active tab's controls. Tab order (from pc64_uui.c):
#   0 Display        : Resolution, Font, UI scale, "Aurora lite"
#   1 Personalization: Theme, Dark mode, Wallpaper, icon flow/sort, snap/lock
#   2 Network        : read-only status + Refresh
#   3 Audio          : Volume, Output device
#   4 Date & Time    : time spinners, Set date, Clock format
#   5 System         : Battery display, Restore session, Lid sleep, Pointer speed
def cp_open_tab(q, tab_idx):
    """Open the Control Panel and select tab tab_idx. Leaves focus on the tab
    strip; a following Tab steps into that tab's first control. The Panel
    reopens on its LAST-viewed tab, so clamp left to Display (tab 0) first, then
    walk right - deterministic regardless of the remembered tab."""
    close_all(q); launch(q, A("control"))
    key(q, "tab"); time.sleep(0.3)                   # focus the tab strip
    for _ in range(6):
        key(q, "left", gap=0.12)                     # clamp at Display (tab 0)
    for _ in range(tab_idx):
        key(q, "right", gap=0.25)                    # walk to the target tab
    time.sleep(0.4)

def sc_controlpanel(q):
    close_all(q)
    launch(q, A("control"))
    shot(q, "controlpanel")                          # opens on the Display tab

def sc_personalization(q):
    cp_open_tab(q, 1)
    shot(q, "cp_personalization")                    # Theme, Dark mode, Wallpaper

def sc_themes(q):
    # Personalization tab -> Theme dropdown (the first control after the strip);
    # Down cycles kThemes live, re-skinning the whole desktop. Order:
    # 0 Aurora Light 1 Aurora Dark 2 UnoDOS 3 Mac OS 7 4 Mac Plus 5 Windows 3.1
    # 6 Amiga 7 C64 8 Apple II 9 NeXTSTEP
    cp_open_tab(q, 1)
    key(q, "tab"); time.sleep(0.3)                   # strip -> Theme dropdown
    shot(q, "theme_aurora_light")
    for tag in ["aurora_dark", "unodos", "macos7", "macplus",
                "win31", "amiga", "c64", "apple2", "next"]:
        key(q, "down"); time.sleep(0.45)
        shot(q, "theme_" + tag)
    for _ in range(9):                               # back to Aurora Light so
        key(q, "up", gap=0.3)                        # later scenes match

def sc_fonts(q):
    # Display tab -> Font dropdown (2nd control: Resolution, Font, ...).
    cp_open_tab(q, 0)
    key(q, "tab", "tab"); time.sleep(0.3)            # strip -> Resolution -> Font
    key(q, "down"); time.sleep(0.5)                  # 1st TTF (Sans)
    shot(q, "font_ttf")
    key(q, "up"); time.sleep(0.4)                    # restore the default face

def sc_resolution(q):
    # Display tab -> Resolution dropdown (1st control after the strip).
    cp_open_tab(q, 0)
    key(q, "tab"); time.sleep(0.3)                   # strip -> Resolution
    key(q, "down"); time.sleep(0.6)
    shot(q, "resolution")
    key(q, "up"); time.sleep(0.6)                    # restore the default mode

def sc_uiscale(q):
    # Display tab -> UI scale dropdown (3rd control after the strip). Each change
    # rescales every font and rebuilds the window, moving focus - so re-open the
    # Panel cleanly for each step (UI scale is a persisted global). Ends back at
    # 100% so later scenes shoot at the normal scale.
    def bump(step):
        cp_open_tab(q, 0)
        key(q, "tab", "tab", "tab"); time.sleep(0.3) # strip -> Res -> Font -> UI scale
        key(q, step); time.sleep(1.2)                # commit + shell rebuild
    bump("down")                                     # 100% -> 125%
    bump("down")                                     # 125% -> 150%
    shot(q, "uiscale")
    bump("up")                                       # 150% -> 125%
    bump("up")                                       # 125% -> 100%

def sc_editor(q):
    close_all(q); launch(q, A("editor"))
    shot(q, "editor")
    # rich text: select all, bold + italic via the Ctrl accelerators
    combo(q, "ctrl", "a"); time.sleep(0.3)
    combo(q, "ctrl", "b"); time.sleep(0.5)
    shot(q, "editor_rich")

def sc_files(q):
    close_all(q); launch(q, A("files"))
    shot(q, "files")
    text(q, "2"); time.sleep(0.6)                    # two-pane commander view
    shot(q, "files_two")

def sc_system(q):
    close_all(q); launch(q, A("system"))
    shot(q, "system")

def sc_clock(q):
    close_all(q); launch(q, A("clock"))
    shot(q, "clock")

def sc_logview(q):
    """The system log, with real activity in it.

    A log viewer photographed on a freshly booted machine shows ONE line -
    unolog announcing itself - which teaches nothing about what the feature
    is for. So open a few documents first; each is a real record the browser
    wrote, not a staged one.

    Deliberately LOCAL documents, no network. The network scenes do not
    currently reproduce (DNS fails on a production build - see the note in
    the manual commit), and a figure that depends on a broken path is a
    figure that regenerates as an error page.

    Two traps this encodes: the address bar has NO select-all, so Ctrl-L
    leaves the caret at the END and typing APPENDS - hence End + backspaces
    before every address; and the level starts at notice, which DROPS the
    info lines the figure exists to show, so More is pressed first."""
    close_all(q)

    def goto(loc, settle=1.4):
        combo(q, "ctrl", "l"); time.sleep(0.35)
        key(q, "end")
        for _ in range(40):
            key(q, "backspace", gap=0.02)
        text(q, loc); key(q, "ret"); time.sleep(settle)

    close_all(q)
    launch(q, A("logview"), settle=2.4)
    # RAISE THE LEVEL FIRST, then generate the traffic. A record dropped
    # for being over the level is gone - turning the level up afterwards
    # shows an empty log and a "dropped 5" counter, which is honest and
    # useless as a figure.
    text(q, "="); time.sleep(0.6)          # More: notice -> info
    launch(q, A("browser"), settle=2.0)
    goto("uno:sample")
    goto("uno:script")
    goto("uno:engine")
    combo(q, "ctrl", "w"); time.sleep(1.2)   # close the browser, log is behind
    shot(q, "logview")

def sc_install(q):
    close_all(q); launch(q, A("install"), settle=2.0)
    shot(q, "install")

def sc_dostris(q):
    close_all(q); launch(q, A("dostris"))
    key(q, "n"); time.sleep(0.5)
    for _ in range(4):
        key(q, "left", gap=0.15); key(q, "spc", gap=0.25)
    time.sleep(0.5)
    shot(q, "dostris")

def sc_pacman(q):
    close_all(q); launch(q, A("pacman"))
    key(q, "n"); time.sleep(0.4)
    for _ in range(3):
        key(q, "right", gap=0.2)
    time.sleep(0.5)
    shot(q, "pacman")

def sc_outlast(q):
    close_all(q); launch(q, A("outlast"))
    key(q, "n"); time.sleep(0.5)
    shot(q, "outlast")

def sc_music(q):
    close_all(q); launch(q, A("music"))
    shot(q, "music")

def sc_tracker(q):
    close_all(q); launch(q, A("tracker"))
    shot(q, "tracker")

def sc_paint(q):
    close_all(q); launch(q, A("paint"))
    shot(q, "paint")

def sc_runner3d(q):
    close_all(q); launch(q, A("runner3d"), settle=2.2)
    time.sleep(1.0)
    shot(q, "runner3d")

def sc_studio(q):
    # The IDE (Start-menu index 14). Greets with SDK\SAMPLE.C, syntax-lit.
    close_all(q); launch(q, A("studio"), settle=2.8)
    shot(q, "studio")
    combo(q, "ctrl", "b"); time.sleep(2.8)           # build -> SAMPLE.UNO
    shot(q, "studio_build")                          # build-output pane
    combo(q, "ctrl", "r"); time.sleep(2.8)           # run the built app
    shot(q, "studio_run")

def sc_studio_ai(q):
    # The AI column needs a wide desktop, so bump the resolution first
    # (Control Panel -> Resolution dropdown -> a bigger mode), then open Studio.
    close_all(q); launch(q, A("control"))
    key(q, "tab", "tab"); time.sleep(0.3)            # focus Resolution dropdown
    key(q, "down", gap=0.5); key(q, "down", gap=0.5) # up two modes; shell reflows
    time.sleep(1.4)
    close_all(q)
    launch(q, A("studio"), settle=2.8)                        # Studio, now wide -> AI column shows
    shot(q, "studio_ai")
    # back to the default resolution so later scenes match
    close_all(q); launch(q, A("control"))
    key(q, "tab", "tab"); time.sleep(0.3)
    key(q, "up", gap=0.5); key(q, "up", gap=0.5)
    time.sleep(1.0); close_all(q)

# ---- the SDK sample programs (the manual's dev-samples.html) ---------------
# The samples are staged onto the shots image as INSTALLED apps - built on the
# host, dropped in APPS\ with a descriptor - and launched from the Start menu
# by id, exactly like every other scene here. Stage them before mkuefi.py:
#
#   gcc -O1 -Wall -o build/ucc_host tools/ucc_host.c apps/ucc.c apps/ucc_x64.c #       -DUCC_KEXPORTS_H='"../build/apps/ucc_kexports.h"'
#   ./build/ucc_host sdk/TIMER.C build/esp/APPS/TIMER.UNO --desc sdk/TIMER.DESC
#   ./build/ucc_host sdk/LIFE.C  build/esp/APPS/LIFE.UNO  --desc sdk/LIFE.DESC
#   python3 tools/mkuno.py pyapp sdk/TODO.PY build/esp/APPS/TODO.UNO #       sdk/TODO.DESC                      # and CHART.PY, GOODNITE.PY likewise
#   python3 tools/mkuefi.py && UNO_DISK=build/unodos-uefi.img ...
#
# build.sh does NOT install them: the samples ship as SOURCE in SDK\, to be
# opened and run in Studio. The descriptors exist so this harness can reach
# them, and they rank into the "other" section at 240+ so the five land at the
# END of the menu and leave every shipped app's index alone.
#
# Why not drive Studio - open the sample, Ctrl-R, photograph what runs? Because
# **a QMP harness cannot click anything in pc64.** QEMU's usb-tablet delivers no
# pointer MOTION to this guest, so every injected mouse-down arrives at the
# framebuffer centre whatever coordinate it was given (pc64/tools/urcui.py
# exists because three lanes hit this independently). Studio's project pane is
# reachable ONLY by clicking it - the File menu has New/Save/Save As and no
# Open, and Tab leaves the pane rather than entering it - so from here there is
# no way to open a file in Studio at all. The first version of this scene
# "worked": the clicks landed in the editor, the arrows walked the caret, and
# Ctrl-R ran the GREETING sample - five figures of SAMPLE.UNO's bouncing ball
# under five different names, and nothing failed.
#
# Files is not the way round either: its panes list NATIVE volumes only, so on
# this image it shows one RAM disk and never the ESP, and its volume picker is
# a dropdown - which is to say, a click.

def _sample_run(q, app_id, settle=3.0):
    """Launch a staged sample from the Start menu, by id like every scene."""
    close_all(q); launch(q, A(app_id), settle=settle)

def sc_sample_timer(q):
    # The keypress needs the app to be up AND focused: sent too early it goes
    # to the launcher, and the figure is a timer sitting at its start value
    # with "Space: start" still on it - which is what the first run captured.
    _sample_run(q, "timer", settle=4.5)
    time.sleep(1.5)
    key(q, "spc", hold=120); time.sleep(4.0)  # start it; the bar drains
    shot(q, "samples_timer")
    close_all(q)

def sc_sample_life(q):
    _sample_run(q, "life")
    time.sleep(3.0)                           # a few dozen generations
    shot(q, "samples_life")
    close_all(q)

def sc_sample_todo(q):
    _sample_run(q, "todo", settle=4.5)
    time.sleep(1.5)
    text(q, "buy milk", gap=0.12); key(q, "ret", gap=0.4)
    text(q, "call mom", gap=0.12); key(q, "ret", gap=0.4)
    key(q, "tab"); time.sleep(0.8)            # check the selected task off
    shot(q, "samples_todo")
    close_all(q)

def sc_sample_chart(q):
    # No DATA.CSV on the boot volume, deliberately: the figure shows the
    # demo-data first launch, header line and all - the state a reader meets.
    _sample_run(q, "chart")
    shot(q, "samples_chart")
    close_all(q)

def sc_sample_goodnite(q):
    # Nobody is signed in, so the figure shows the graceful-denial path -
    # which is the sample's actual point.
    _sample_run(q, "goodnite", settle=8.0)   # let the script walk its steps
    shot(q, "samples_goodnite")
    close_all(q)


def uc_run(q, name, settle=0.9):
    """Run a UnoCode command by NAME through the command palette.

    Every UnoCode scene drives the product this way rather than by key chord,
    for the same reason the app roster is looked up by id: a chord's meaning
    depends on where the keyboard focus already is. Ctrl+Shift+E is three-state
    (show the side bar / focus it / hide it), so one press cannot mean "show
    it" from an unknown state - and the state carries over between scenes,
    because closing UnoCode's window does not throw its layout away."""
    combo(q, "ctrl", "shift", "p"); time.sleep(0.6)
    text(q, name); time.sleep(0.6)
    key(q, "ret"); time.sleep(settle)


def _unocode(q, settle=3.6):
    """Open UnoCode with the side bar, the panel and the focus in ONE state.

    `Reset Layout` is idempotent by construction - side bar on and showing the
    Explorer, panel off, focus in the editor - which is what makes a scene's
    figure a function of the scene rather than of the scene before it."""
    close_all(q); launch(q, A("unocode"), settle=settle)
    uc_run(q, "Reset Layout")
    return q


def _uc_open(q, path):
    """Open a file by path through the integrated terminal.

    Not through Ctrl+P: Go to File lists the workspace FOLDER, and the SDK
    samples live a level down from it. The terminal takes a path."""
    uc_run(q, "Toggle Terminal")
    text(q, "open " + path); key(q, "ret"); time.sleep(1.8)
    uc_run(q, "Reset Layout")                            # panel away again

def sc_unocode(q):
    # The workbench as it opens: activity bar, Explorer, the welcome document.
    _unocode(q)
    shot(q, "unocode")
    combo(q, "ctrl", "shift", "p"); time.sleep(0.6)      # the command palette
    text(q, "theme"); time.sleep(0.8)
    shot(q, "unocode_palette")
    key(q, "esc"); time.sleep(0.4)
    close_all(q)

def sc_unocode_editor(q):
    # A real source file: grammar highlighting, minimap, breadcrumbs, gutter.
    _unocode(q)
    _uc_open(q, "SDK\\SAMPLE.C")
    shot(q, "unocode_editor")
    combo(q, "ctrl", "f"); time.sleep(0.6)               # find, with a count
    text(q, "static"); time.sleep(1.0)
    shot(q, "unocode_find")
    key(q, "esc"); time.sleep(0.4)
    combo(q, "ctrl", "spc"); time.sleep(1.2)             # IntelliSense
    shot(q, "unocode_suggest")
    key(q, "esc"); time.sleep(0.4)
    close_all(q)

def sc_unocode_ext(q):
    """The extension story in two figures: what is installed, and one of them
    actually running. The second is the whole host in one picture - manifest
    read, activation event fired, MAIN.JS evaluated, registerCommand handler
    invoked, and the notification it raised on screen."""
    _unocode(q)
    uc_run(q, "Show Extensions", settle=1.2)
    shot(q, "unocode_extensions")
    uc_run(q, "Say Hello", settle=2.0)                   # activates + runs it
    shot(q, "unocode_ext_run")
    close_all(q)

def sc_unocode_theme(q):
    # Nord, contributed by an extension that ships no JavaScript at all.
    _unocode(q)
    _uc_open(q, "SDK\\SAMPLE.C")
    uc_run(q, "Color Theme", settle=1.0)
    text(q, "Nord"); time.sleep(0.8)
    key(q, "ret"); time.sleep(1.8)
    shot(q, "unocode_theme")
    # Put the default back. The theme is PERSISTED to settings.json, so a scene
    # that left it would photograph every later figure - including any rerun of
    # an unrelated one - in somebody else's colours.
    uc_run(q, "Color Theme", settle=1.0)
    text(q, "Dark"); time.sleep(0.8)
    key(q, "ret"); time.sleep(1.6)
    close_all(q)

def sc_unocode_terminal(q):
    _unocode(q)
    uc_run(q, "Toggle Terminal")
    text(q, "help"); key(q, "ret"); time.sleep(1.0)
    text(q, "ext"); key(q, "ret"); time.sleep(1.0)
    shot(q, "unocode_terminal")
    close_all(q)


def sc_duum(q):
    """Duum, the Python Doom engine. Slow to start by nature: PYRT boots, then
    the WAD is parsed and the textures composed, which is tens of seconds of
    emulated work before the first frame exists. Everything here waits on that
    rather than assuming it - a short settle photographs a blank canvas and
    files it as the game."""
    close_all(q); launch(q, A("duum"), settle=6.0)
    time.sleep(45.0)                      # PYRT start + WAD parse + first frame
    shot(q, "duum_start")                 # E1M1 as the game opens
    key(q, "up", gap=0.25); key(q, "up", gap=0.25)   # walk forward
    key(q, "right", gap=0.25)                        # turn
    time.sleep(6.0)
    shot(q, "duum_play")                  # a different view, so the shot proves motion
    close_all(q)


def _uof_open(q, name):
    """Open a document by NAME in UnoWord/UnoCalc's shared Open dialog.

    Typed into the File-name field rather than picked off the list: arrow keys
    never reach that list (see tools/demo/scenes.py uof_open_row), so the
    alternative is clicking a row whose position depends on what else is on
    the disk. A typed name does not care how many files are in the root."""
    combo(q, "ctrl", "o"); time.sleep(1.2)
    click(q, 637, 359)                    # the File name field
    text(q, name); time.sleep(0.3)
    click(q, 747, 361); time.sleep(2.5)   # Open


def sc_office_duum(q):
    """The three-window composite: a real Word document, a real spreadsheet,
    and Duum, all on one desktop at once. Duum goes FIRST and stays behind:
    it is the slowest to start (PYRT + the WAD parse) and it is the window the
    other two are meant to sit in front of."""
    # ORDER MATTERS, and not for looks. Launching anything while Duum renders
    # loses the first keys of the Start-menu walk - reproducibly, twice, two
    # rows short, which opens SSH under UnoCalc's name. So the quiet windows
    # are opened first, on an idle desktop, and Duum goes last. It lands on
    # top, which is also the composition this shot wants.
    close_all(q)
    launch(q, A("uocalc"), settle=3.0)
    _uof_open(q, "BUDGET.XLS")
    launch(q, A("uoword"), settle=3.0)
    _uof_open(q, "RESUME.DOC")
    # Snap the two documents into the left quarters BEFORE Duum exists, while
    # the desktop is quiet and Alt-Tab has only two windows to walk. Snapping
    # is Alt+arrow (see the manual's window chapter); Start > Windows > Tile
    # would be tidier but it lives in the menu's second column, which the
    # pointer has to reach, and the pointer is the one input this environment
    # does not deliver reliably.
    combo(q, "alt", "left"); time.sleep(0.6)
    combo(q, "alt", "up"); time.sleep(1.0)          # UnoWord -> top-left
    combo(q, "alt", "tab"); time.sleep(1.2)         # focus UnoCalc
    combo(q, "alt", "left"); time.sleep(0.6)
    combo(q, "alt", "down"); time.sleep(1.0)        # UnoCalc -> bottom-left
    launch(q, A("duum"), settle=6.0)
    time.sleep(45.0)                      # PYRT start + WAD parse + first frame
    combo(q, "alt", "right"); time.sleep(1.5)       # Duum -> right half
    shot(q, "office_duum")


def sc_browser_disk(q):
    close_all(q); launch(q, A("browser"), settle=2.0)
    shot(q, "browser_files")

def _browser_open(q, row, tag, settle=1.6):
    # Fresh browser each time. Entering the list from the address bar lands on
    # row 1 (Sample.html); Up from row 0 jumps BACK to the address bar, so we
    # navigate RELATIVE to row 1 and never go above row 0.
    close_all(q); launch(q, A("browser"), settle=2.0)
    key(q, "down"); time.sleep(0.3)                  # address bar -> list row 1
    delta = row - 1
    for _ in range(abs(delta)):
        key(q, "up" if delta < 0 else "down", gap=0.14)
    key(q, "ret"); time.sleep(settle); shot(q, tag)

def sc_browser_docs(q):
    _browser_open(q, 0, "browser_markdown")          # Welcome.md  (Markdown)
    _browser_open(q, 1, "browser_html")              # Sample.html (HTML+CSS)
    _browser_open(q, 2, "browser_js", settle=2.0)    # Script.html (JavaScript)

def sc_cp_network(q):
    # The Control Panel's Network tab (the standalone Network app was dropped
    # 2026-07-26). Needs a NIC (run with UNO_NIC=1). pc64 binds the NIC lazily -
    # on first network use - so bring the link up by loading a page in the
    # Browser first, then read the live status in the tab (Refresh to update it).
    close_all(q); launch(q, A("browser"), settle=2.0)          # Browser
    text(q, "http://%s/" % DOCS_HOST); time.sleep(0.3)
    key(q, "ret"); time.sleep(6.0)                   # DHCP+DNS+GET brings the link up
    cp_open_tab(q, 2)                                # Display -> ... -> Network
    key(q, "tab"); time.sleep(0.2)                   # strip -> Refresh button
    key(q, "ret"); time.sleep(1.0)                   # click Refresh -> fresh status
    shot(q, "cp_network")

# The two live-network figures fetch example.com, the canonical illustrative
# domain, and that is what the manual should show. But the scene can only be
# regenerated on a network whose resolver answers for it: this box's does NOT
# (NXDOMAIN for example.com while google.com and cloudflare.com resolve), which
# in 2026-08 read as "DNS is broken on production builds" and cost a filed
# request before anyone checked the host. So the host is overridable: set
# UNO_DOCS_HOST to regenerate these figures from somewhere else without
# editing the scene, and note in the commit that the figure now shows a
# different site.
DOCS_HOST = os.environ.get("UNO_DOCS_HOST", "example.com")

def sc_browser_http(q):
    close_all(q); launch(q, A("browser"), settle=2.0)
    text(q, "http://%s/" % DOCS_HOST); time.sleep(0.3)
    key(q, "ret"); time.sleep(5.0)                   # DHCP+DNS+TCP+GET+render
    shot(q, "browser_http")

def sc_browser_https(q):
    close_all(q); launch(q, A("browser"), settle=2.0)
    text(q, "https://%s/" % DOCS_HOST); time.sleep(0.3)
    key(q, "ret"); time.sleep(10.0)                  # + TLS 1.2 handshake
    shot(q, "browser_https")

# ---- 2026-08 additions: window management and the SSH client ---------------
def sc_winsnap(q):
    """A window snapped to half the screen, with the desktop pager beside the
    Start button. Both are new: drag-to-edge snapping and four virtual
    desktops."""
    close_all(q); launch(q, A("editor"), settle=2.0)          # Editor
    combo(q, "alt", "left"); time.sleep(1.0)        # snap to the left half
    shot(q, "winsnap")

def sc_desktops(q):
    """Desktop 2, reached with Ctrl+F2. The pager cell for the current desktop
    is filled, and a dot marks any desktop that has windows on it."""
    close_all(q); launch(q, A("editor"), settle=2.0)
    combo(q, "ctrl", "f2"); time.sleep(1.2)
    shot(q, "desktops")
    combo(q, "ctrl", "f1"); time.sleep(0.6)

def sc_switcher(q):
    """The Alt-Tab switcher overlay. F2 drives the same overlay from a keyboard
    with no working Alt, which is why it exists."""
    close_all(q); launch(q, A("editor"), settle=2.0); launch(q, A("files"), settle=2.0)
    key(q, "f2"); time.sleep(0.5)
    shot(q, "switcher")
    key(q, "esc"); time.sleep(0.4)

def _sc_ssh_at(q, idx, tag):
    close_all(q); launch(q, idx, settle=2.5)
    shot(q, tag)

# The SSH client sits last in the launcher, at 17. Probed rather than assumed:
# the first guess was 19 and opened nothing at all.
def sc_ssh(q): _sc_ssh_at(q, A("ssh"), "ssh")


# ---- UnoOffice ------------------------------------------------------------
# The suite ships as three .UNO modules and each opens on an empty document, so
# every scene types something first: a manual figure of a blank page teaches
# nothing. Keyboard only - these apps are driven here exactly as someone without
# a mouse would drive them.
def sc_uoword(q):
    close_all(q); launch(q, A("uoword"), settle=3.0)
    shot(q, "uoword")
    text(q, "UnoDOS runs a word processor.")
    time.sleep(0.4)
    shot(q, "uoword_typed")

def sc_uocalc(q):
    close_all(q); launch(q, A("uocalc"), settle=3.0)
    shot(q, "uocalc")
    # a formula, so the figure shows a RESULT and the formula bar together -
    # the one thing that tells a spreadsheet apart from a grid of text
    text(q, "12"); key(q, "ret", gap=0.25)
    text(q, "30"); key(q, "ret", gap=0.25)
    text(q, "=A1+A2"); key(q, "ret", gap=0.6)
    key(q, "up", gap=0.6)                    # back onto the formula cell, so the
    time.sleep(0.8)                          # figure shows result AND formula bar
    shot(q, "uocalc_formula")

def sc_uoshow(q):
    close_all(q); launch(q, A("uoshow"), settle=3.0)
    shot(q, "uoshow")

def sc_unoamp(q):
    close_all(q); launch(q, A("unoamp"), settle=2.4)
    shot(q, "unoamp")

def sc_appliances(q):
    """The appliance manager's list view (APPS\\VMGR.UNO).

    The list, not the console: a console figure needs a guest, and starting one
    needs a kernel staged on the volume, which this capture image does not
    carry. The status line under the list is the useful part anyway - on a
    machine that cannot host a guest it says which capability is missing, and
    QEMU without `-cpu ...,+vmx` is exactly such a machine."""
    close_all(q); launch(q, A("vmgr"), settle=2.6)
    shot(q, "appliances")


SCENES = {
    "winsnap": sc_winsnap, "desktops": sc_desktops, "switcher": sc_switcher,
    "ssh": sc_ssh,
    "uoword": sc_uoword, "uocalc": sc_uocalc, "uoshow": sc_uoshow,
    "unoamp": sc_unoamp, "appliances": sc_appliances,
    "desktop": sc_desktop, "startmenu": sc_startmenu, "controlpanel": sc_controlpanel,
    "personalization": sc_personalization,
    "themes": sc_themes, "fonts": sc_fonts, "resolution": sc_resolution,
    "uiscale": sc_uiscale,
    "editor": sc_editor, "files": sc_files, "system": sc_system, "clock": sc_clock,
    "logview": sc_logview,
    "install": sc_install, "dostris": sc_dostris, "pacman": sc_pacman,
    "outlast": sc_outlast, "music": sc_music, "tracker": sc_tracker,
    "paint": sc_paint, "runner3d": sc_runner3d,
    "studio": sc_studio, "studio_ai": sc_studio_ai,
    "sample_timer": sc_sample_timer, "sample_life": sc_sample_life,
    "sample_todo": sc_sample_todo, "sample_chart": sc_sample_chart,
    "sample_goodnite": sc_sample_goodnite,
    "unocode": sc_unocode, "unocode_editor": sc_unocode_editor,
    "unocode_ext": sc_unocode_ext, "unocode_theme": sc_unocode_theme,
    "unocode_terminal": sc_unocode_terminal,
    "duum": sc_duum, "office_duum": sc_office_duum,
    "browser_disk": sc_browser_disk,
    "browser_docs": sc_browser_docs, "cp_network": sc_cp_network,
    "browser_http": sc_browser_http, "browser_https": sc_browser_https,
}
CORE = list(SCENES.keys())

# Scenes that leave something on screen, and therefore go LAST whatever order
# they were asked for. `close_all` is Ctrl-W, which the shell refuses on a
# UI_WIN_BARE window - the rule that stops it closing the desktop and the
# taskbar - and UnoAmp's three windows are BARE because the Winamp skin draws
# its own chrome. So once `unoamp` has run, the player is on screen for the rest
# of the boot: it put its dark chassis across the middle of `desktop.png` and a
# taskbar chip on every figure after it, in a run that reported no failures.
# Filed to the toolkits lane; deferring the scene is the fix that is available
# here, and it is a real one - nothing else in the set leaves a window.
DEFERRED = ["unoamp"]


def main():
    global MENU
    want = sys.argv[1:] or CORE
    want = ([w for w in want if w not in DEFERRED] +
            [w for w in want if w in DEFERRED])
    MENU = load_roster()
    use_nic = os.environ.get("UNO_NIC") == "1"
    full = os.environ.get("UNO_NETDEV")              # full SLIRP string override
    cpu = ["-cpu", "max"] if full else []            # RDRAND for the TLS test
    if full:
        net = ["-netdev", full, "-device", "e1000,netdev=n0"]
    elif use_nic:
        net = ["-netdev", "user,id=n0", "-device", "e1000,netdev=n0"]
    else:
        net = ["-nic", "none"]
    # The boot disk. By default QEMU FAKES a filesystem out of build/esp
    # (vvfat), which is fine for reading but is not a real FAT32: writes are
    # unreliable, so any scene that WRITES and then reads back - Studio's
    # build-and-run being the one that matters - can fail here for reasons the
    # product does not have. UNO_DISK=<raw image> boots the real GPT+FAT32
    # image from tools/mkuefi.py instead, as usb-storage, the way
    # tools/diskboot_test.py does.
    disk_img = os.environ.get("UNO_DISK")
    if disk_img and os.environ.get("UNO_DISK_IF") == "sata":
        # The same image on the SATA controller instead of USB. It matters for
        # more than plumbing: over usb-storage the volume is a FIRMWARE one, so
        # `uno_fs_*` (Studio, the module loader) sees it but FILES does not -
        # its panes list native volumes only, which is why a keyboard-driven
        # Files scene finds nothing but the RAM disk. Bound to the native AHCI
        # driver it is a native volume and Files can open it.
        print("disk: %s (real FAT32 over AHCI)" % disk_img, flush=True)
        disk = ["-drive", "format=raw,file=" + disk_img]
    elif disk_img:
        print("disk: %s (real FAT32 over usb-storage)" % disk_img, flush=True)
        disk = ["-drive", "format=raw,if=none,id=stick,file=" + disk_img,
                "-device", "usb-storage,drive=stick"]
    else:
        disk = ["-drive", "format=vvfat,file=fat:rw:build/esp"]
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-device", "qemu-xhci", "-device", "usb-tablet",
        # an HD Audio device, so the System window's Audio line shows the real
        # PCM backend (the "none" audiodev just swallows the samples headless)
        "-audiodev", "none,id=snd0",
        "-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-debugcon", "file:build/ovmf.log", "-global", "isa-debugcon.iobase=0x402",
    ] + disk + cpu + net)
    try:
        q = Qmp(QMP_SOCK)
        print("qemu up; waiting for boot...", flush=True)
        if "splash" in want:
            want = [w for w in want if w != "splash"]
            # The return value is the whole point of wait_splash: on timeout the
            # guest has not lit the GOP yet, and capturing anyway files QEMU's
            # own "Guest has not initialized the display (yet)." card in the
            # manual as though it were the UnoDOS splash. That shipped once
            # (fixed 2026-08-20), and it survived because nothing looked at the
            # picture afterwards. Fail loudly instead: a missing figure is
            # noticed, a wrong one is not.
            if not wait_splash(q):
                raise RuntimeError(
                    "splash never appeared within the timeout - the guest had "
                    "not initialised the display, so no shot was taken")
            time.sleep(0.6); shot(q, "splash")
            time.sleep(15.0)
        else:
            time.sleep(18)
        # Before anything is captured, not after: a mislabelled figure is only
        # ever found by someone reading the manual, and every scene below this
        # line depends on the roster being the menu the machine actually has.
        close_all(q)
        count_menu_rows(q, len(MENU))
        for name in want:
            if name not in SCENES:
                print("?? unknown scene", name); continue
            print("=== scene:", name, flush=True)
            try:
                SCENES[name](q)
            except Exception as e:
                print("!! scene %s failed: %r" % (name, e), flush=True)
    finally:
        try:
            q.cmd("quit")
        except Exception:
            qemu.kill()
        qemu.wait(timeout=10)


if __name__ == "__main__":
    main()

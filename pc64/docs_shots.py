#!/usr/bin/env python3
"""Capture the UnoDOS/pc64 screenshots the user manual needs.

Self-contained QMP driver (same technique as pc64/harness.py): boots the real
ESP under QEMU + OVMF headless, drives the desktop over QMP, and dumps the GOP
surface to shots/manual/<tag>.png at each scene.

  python3 docs_shots.py [scene ...]     run named scenes (default: all core)

The desktop is deterministic, so the Start-menu order is fixed - but it MOVES
when an app is added, and nothing here fails when it does: the harness launches
the next app along and captures it under the old name. So the order lives in
named constants below (A_EDITOR, A_UOCALC, ...), measured off the launcher
rather than derived, and those names are what the scenes use.
Launch app N: Ctrl-Esc, Down*N, Enter.  Close focused window: Ctrl-W.
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
        ";": "semicolon", "?": "shift+slash", "'": "apostrophe"}


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


def launch(q, idx, settle=1.6):
    """Open Start menu and pick app `idx` by keyboard."""
    combo(q, "ctrl", "esc"); time.sleep(0.6)
    for _ in range(idx):
        key(q, "down", gap=0.09)
    key(q, "ret"); time.sleep(settle)


# ---------------------------------------------------------------- app indices
# Start-menu order = the app enum order in pc64_uui.c, minus the hidden slots
# (the Studio-built app and a running Python app appear only once they exist).
# MEASURED off the launcher, not derived: `probe` shots at Down x15/18/20.
#
# This list moved on 2026-08-04 when UnoAmp was added at 7, which pushed every
# game and tool down by one. Nothing failed - the harness happily launched the
# NEXT app along and captured it under the old name, so `tracker.png` would
# have shipped a picture of Paint. Hence names, not numbers, below.
A_CONTROL, A_EDITOR, A_FILES, A_SYSTEM, A_CLOCK, A_INSTALL = 0, 1, 2, 3, 4, 5
A_MUSIC, A_UNOAMP = 6, 7
A_DOSTRIS, A_PACMAN, A_OUTLAST, A_TRACKER, A_PAINT = 8, 9, 10, 11, 12
A_RUNNER3D, A_BROWSER, A_STUDIO, A_PHOTOS, A_SSH = 13, 14, 15, 16, 17
A_UOWORD, A_UOCALC, A_UOSHOW = 18, 19, 20
A_LOGVIEW = 21                   # System Log; appended, so 0..20 held

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
    close_all(q); launch(q, A_CONTROL)
    key(q, "tab"); time.sleep(0.3)                   # focus the tab strip
    for _ in range(6):
        key(q, "left", gap=0.12)                     # clamp at Display (tab 0)
    for _ in range(tab_idx):
        key(q, "right", gap=0.25)                    # walk to the target tab
    time.sleep(0.4)

def sc_controlpanel(q):
    close_all(q)
    launch(q, A_CONTROL)
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
    close_all(q); launch(q, A_EDITOR)
    shot(q, "editor")
    # rich text: select all, bold + italic via the Ctrl accelerators
    combo(q, "ctrl", "a"); time.sleep(0.3)
    combo(q, "ctrl", "b"); time.sleep(0.5)
    shot(q, "editor_rich")

def sc_files(q):
    close_all(q); launch(q, A_FILES)
    shot(q, "files")
    text(q, "2"); time.sleep(0.6)                    # two-pane commander view
    shot(q, "files_two")

def sc_system(q):
    close_all(q); launch(q, A_SYSTEM)
    shot(q, "system")

def sc_clock(q):
    close_all(q); launch(q, A_CLOCK)
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
    launch(q, A_LOGVIEW, settle=2.4)
    # RAISE THE LEVEL FIRST, then generate the traffic. A record dropped
    # for being over the level is gone - turning the level up afterwards
    # shows an empty log and a "dropped 5" counter, which is honest and
    # useless as a figure.
    text(q, "="); time.sleep(0.6)          # More: notice -> info
    launch(q, A_BROWSER, settle=2.0)
    goto("uno:sample")
    goto("uno:script")
    goto("uno:engine")
    combo(q, "ctrl", "w"); time.sleep(1.2)   # close the browser, log is behind
    shot(q, "logview")

def sc_install(q):
    close_all(q); launch(q, A_INSTALL, settle=2.0)
    shot(q, "install")

def sc_dostris(q):
    close_all(q); launch(q, A_DOSTRIS)
    key(q, "n"); time.sleep(0.5)
    for _ in range(4):
        key(q, "left", gap=0.15); key(q, "spc", gap=0.25)
    time.sleep(0.5)
    shot(q, "dostris")

def sc_pacman(q):
    close_all(q); launch(q, A_PACMAN)
    key(q, "n"); time.sleep(0.4)
    for _ in range(3):
        key(q, "right", gap=0.2)
    time.sleep(0.5)
    shot(q, "pacman")

def sc_outlast(q):
    close_all(q); launch(q, A_OUTLAST)
    key(q, "n"); time.sleep(0.5)
    shot(q, "outlast")

def sc_music(q):
    close_all(q); launch(q, A_MUSIC)
    shot(q, "music")

def sc_tracker(q):
    close_all(q); launch(q, A_TRACKER)
    shot(q, "tracker")

def sc_paint(q):
    close_all(q); launch(q, A_PAINT)
    shot(q, "paint")

def sc_runner3d(q):
    close_all(q); launch(q, A_RUNNER3D, settle=2.2)
    time.sleep(1.0)
    shot(q, "runner3d")

def sc_studio(q):
    # The IDE (Start-menu index 14). Greets with SDK\SAMPLE.C, syntax-lit.
    close_all(q); launch(q, A_STUDIO, settle=2.8)
    shot(q, "studio")
    combo(q, "ctrl", "b"); time.sleep(2.8)           # build -> SAMPLE.UNO
    shot(q, "studio_build")                          # build-output pane
    combo(q, "ctrl", "r"); time.sleep(2.8)           # run the built app
    shot(q, "studio_run")

def sc_studio_ai(q):
    # The AI column needs a wide desktop, so bump the resolution first
    # (Control Panel -> Resolution dropdown -> a bigger mode), then open Studio.
    close_all(q); launch(q, A_CONTROL)
    key(q, "tab", "tab"); time.sleep(0.3)            # focus Resolution dropdown
    key(q, "down", gap=0.5); key(q, "down", gap=0.5) # up two modes; shell reflows
    time.sleep(1.4)
    close_all(q)
    launch(q, A_STUDIO, settle=2.8)                        # Studio, now wide -> AI column shows
    shot(q, "studio_ai")
    # back to the default resolution so later scenes match
    close_all(q); launch(q, A_CONTROL)
    key(q, "tab", "tab"); time.sleep(0.3)
    key(q, "up", gap=0.5); key(q, "up", gap=0.5)
    time.sleep(1.0); close_all(q)

def sc_browser_disk(q):
    close_all(q); launch(q, A_BROWSER, settle=2.0)
    shot(q, "browser_files")

def _browser_open(q, row, tag, settle=1.6):
    # Fresh browser each time. Entering the list from the address bar lands on
    # row 1 (Sample.html); Up from row 0 jumps BACK to the address bar, so we
    # navigate RELATIVE to row 1 and never go above row 0.
    close_all(q); launch(q, A_BROWSER, settle=2.0)
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
    close_all(q); launch(q, A_BROWSER, settle=2.0)          # Browser
    text(q, "http://example.com/"); time.sleep(0.3)
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
    close_all(q); launch(q, A_BROWSER, settle=2.0)
    text(q, "http://%s/" % DOCS_HOST); time.sleep(0.3)
    key(q, "ret"); time.sleep(5.0)                   # DHCP+DNS+TCP+GET+render
    shot(q, "browser_http")

def sc_browser_https(q):
    close_all(q); launch(q, A_BROWSER, settle=2.0)
    text(q, "https://%s/" % DOCS_HOST); time.sleep(0.3)
    key(q, "ret"); time.sleep(10.0)                  # + TLS 1.2 handshake
    shot(q, "browser_https")

# ---- 2026-08 additions: window management and the SSH client ---------------
def sc_winsnap(q):
    """A window snapped to half the screen, with the desktop pager beside the
    Start button. Both are new: drag-to-edge snapping and four virtual
    desktops."""
    close_all(q); launch(q, A_EDITOR, settle=2.0)          # Editor
    combo(q, "alt", "left"); time.sleep(1.0)        # snap to the left half
    shot(q, "winsnap")

def sc_desktops(q):
    """Desktop 2, reached with Ctrl+F2. The pager cell for the current desktop
    is filled, and a dot marks any desktop that has windows on it."""
    close_all(q); launch(q, A_EDITOR, settle=2.0)
    combo(q, "ctrl", "f2"); time.sleep(1.2)
    shot(q, "desktops")
    combo(q, "ctrl", "f1"); time.sleep(0.6)

def sc_switcher(q):
    """The Alt-Tab switcher overlay. F2 drives the same overlay from a keyboard
    with no working Alt, which is why it exists."""
    close_all(q); launch(q, A_EDITOR, settle=2.0); launch(q, A_FILES, settle=2.0)
    key(q, "f2"); time.sleep(0.5)
    shot(q, "switcher")
    key(q, "esc"); time.sleep(0.4)

def _sc_ssh_at(q, idx, tag):
    close_all(q); launch(q, idx, settle=2.5)
    shot(q, tag)

# The SSH client sits last in the launcher, at 17. Probed rather than assumed:
# the first guess was 19 and opened nothing at all.
def sc_ssh(q): _sc_ssh_at(q, A_SSH, "ssh")


# ---- UnoOffice ------------------------------------------------------------
# The suite ships as three .UNO modules and each opens on an empty document, so
# every scene types something first: a manual figure of a blank page teaches
# nothing. Keyboard only - these apps are driven here exactly as someone without
# a mouse would drive them.
def sc_uoword(q):
    close_all(q); launch(q, A_UOWORD, settle=3.0)
    shot(q, "uoword")
    text(q, "UnoDOS runs a word processor.")
    time.sleep(0.4)
    shot(q, "uoword_typed")

def sc_uocalc(q):
    close_all(q); launch(q, A_UOCALC, settle=3.0)
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
    close_all(q); launch(q, A_UOSHOW, settle=3.0)
    shot(q, "uoshow")

def sc_unoamp(q):
    close_all(q); launch(q, A_UNOAMP, settle=2.4)
    shot(q, "unoamp")


SCENES = {
    "winsnap": sc_winsnap, "desktops": sc_desktops, "switcher": sc_switcher,
    "ssh": sc_ssh,
    "uoword": sc_uoword, "uocalc": sc_uocalc, "uoshow": sc_uoshow,
    "unoamp": sc_unoamp,
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
    "browser_disk": sc_browser_disk,
    "browser_docs": sc_browser_docs, "cp_network": sc_cp_network,
    "browser_http": sc_browser_http, "browser_https": sc_browser_https,
}
CORE = list(SCENES.keys())


def main():
    want = sys.argv[1:] or CORE
    use_nic = os.environ.get("UNO_NIC") == "1"
    full = os.environ.get("UNO_NETDEV")              # full SLIRP string override
    cpu = ["-cpu", "max"] if full else []            # RDRAND for the TLS test
    if full:
        net = ["-netdev", full, "-device", "e1000,netdev=n0"]
    elif use_nic:
        net = ["-netdev", "user,id=n0", "-device", "e1000,netdev=n0"]
    else:
        net = ["-nic", "none"]
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", "format=vvfat,file=fat:rw:build/esp",
        "-device", "qemu-xhci", "-device", "usb-tablet",
        # an HD Audio device, so the System window's Audio line shows the real
        # PCM backend (the "none" audiodev just swallows the samples headless)
        "-audiodev", "none,id=snd0",
        "-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-debugcon", "file:build/ovmf.log", "-global", "isa-debugcon.iobase=0x402",
    ] + cpu + net)
    try:
        q = Qmp(QMP_SOCK)
        print("qemu up; waiting for boot...", flush=True)
        if "splash" in want:
            want = [w for w in want if w != "splash"]
            wait_splash(q); time.sleep(0.6); shot(q, "splash")
            time.sleep(15.0)
        else:
            time.sleep(18)
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

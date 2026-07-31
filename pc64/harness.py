#!/usr/bin/env python3
"""UnoDOS/pc64 scripted verification harness - QEMU + OVMF, fully headless.

The pc64 counterpart of the family's harness.py pattern - but where the
console ports need a ROM-free CPU-core harness (no headless grab under RDP),
QEMU is natively scriptable: boot the ESP under OVMF with no display, drive
the desktop over QMP (send-key), and capture the GOP surface (screendump)
into shots/*.png at each step.

  python3 harness.py            boot -> desktop shot, then the scripted
                                keyboard pass: SysInfo, Clock, Notepad
                                (typing), Dostris (playing) - a shot each.
  python3 harness.py boot       boot -> desktop shot only.
  python3 harness.py wm_a       WM phase A gate (live drag, double-click
                                maximize, per-app geometry across a reboot).
  python3 harness.py wm_d       WM phase D gate (Alt-Tab, snap, show desktop).
"""
import json, os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
sys.path.insert(0, os.path.join(HERE, "tools"))
import ppm2png                                # read_ppm/write_png, stdlib only

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
QMP_SOCK  = "/tmp/unodos-pc64-qmp.sock"   # NOT under build/: a Windows-mounted
                                          # drvfs tree cannot host unix sockets


class Qmp:
    def __init__(self, path, timeout=30):
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
        self.recv()                          # greeting
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


def keys(q, *names, hold=40, gap=0.12):
    for n in names:
        q.cmd("send-key", keys=[{"type": "qcode", "data": n}], **{"hold-time": hold})
        time.sleep(gap)


def text(q, s):
    QMAP = {" ": "spc", ".": "dot", ",": "comma", "-": "minus", "/": "slash"}
    for ch in s:
        if ch.isupper():
            q.cmd("send-key", keys=[{"type": "qcode", "data": "shift"},
                                    {"type": "qcode", "data": ch.lower()}])
        else:
            q.cmd("send-key", keys=[{"type": "qcode", "data": QMAP.get(ch, ch)}])
        time.sleep(0.1)


# An absolute pointer's axes are normalised 0..32767 over the WHOLE guest
# display, so a click scripted in screen pixels has to know the LIVE GOP mode.
# These divisors were hardcoded 640x480 while the shell boots 1280x800 under
# OVMF: every scripted mouse coordinate landed at half scale, so a drag aimed
# at a title bar quietly hit bare desktop and everything downstream "passed" by
# dragging nothing. probe_screen() reads the real mode off a screendump header.
#
# SCALE is the present path's integer zoom: the shell renders a 640x400
# framebuffer and blits it doubled into the 1280x800 mode, so a screendump
# pixel is NOT a guest framebuffer pixel and pointer deltas (which are in
# framebuffer pixels) have to be divided by it.
SCREEN_W, SCREEN_H, SCALE = 640, 480, 1


def probe_scale(img, wh):
    """Detect the present path's integer zoom from the pixels themselves: a
    doubled frame is exactly 2x2-blocky. Requires real variation in the sample
    so a blank screen cannot answer "any scale you like"."""
    w, h = wh
    def pix(x, y):
        i = (y * w + x) * 3
        return img[i], img[i + 1], img[i + 2]
    for s in (4, 3, 2):
        if w % s or h % s:
            continue
        ok, seen = True, set()
        for y in range(0, h - s, 29):
            for x in range(0, w - s, 31):
                bx, by = x - x % s, y - y % s
                base = pix(bx, by)
                seen.add(base)
                for dy in range(s):
                    for dx in range(s):
                        if pix(bx + dx, by + dy) != base:
                            ok = False
                            break
                    if not ok:
                        break
                if not ok:
                    break
            if not ok:
                break
        if ok and len(seen) > 8:
            return s
    return 1


def probe_screen(q):
    global SCREEN_W, SCREEN_H, SCALE
    ppm = "shots/_probe.ppm"
    q.cmd("screendump", filename=ppm)
    time.sleep(0.4)
    w, h, rgb = ppm2png.read_ppm(ppm)
    os.remove(ppm)
    SCREEN_W, SCREEN_H = w, h
    SCALE = probe_scale(rgb, (w, h))
    print("screen: %dx%d (guest fb %dx%d, zoom %dx)"
          % (w, h, w // SCALE, h // SCALE, SCALE))
    return w, h


def mouse_move(q, x, y):
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / SCREEN_W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / SCREEN_H)}}])
    time.sleep(0.1)


def mouse_btn(q, down):
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"down": down, "button": "left"}}])
    time.sleep(0.1)


def click(q, x, y):
    mouse_move(q, x, y)
    mouse_btn(q, True)
    mouse_btn(q, False)


def shot(q, tag):
    ppm = "shots/%s.ppm" % tag
    q.cmd("screendump", filename=ppm)
    time.sleep(0.4)
    subprocess.run([sys.executable, "tools/ppm2png.py", ppm, "shots/%s.png" % tag],
                   check=True)
    os.remove(ppm)
    print("shot: shots/%s.png" % tag)


# ---- window-manager scenarios (docs/WM-MODERN-SPEC.md) --------------------
# Held modifiers need press and release as SEPARATE events: send-key always
# releases everything it pressed, so it can never hold Alt across two Tab
# steps, which is the whole point of an Alt-Tab test.

def key_evt(q, name, down):
    q.cmd("input-send-event", events=[
        {"type": "key", "data": {"down": down,
                                 "key": {"type": "qcode", "data": name}}}])


def tap(q, name, gap=0.15):
    key_evt(q, name, True)
    time.sleep(0.05)
    key_evt(q, name, False)
    time.sleep(gap)


def start_app(q, n, wait=1.8):
    """Open menu entry n through the Start menu (menu order = app order)."""
    q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                            {"type": "qcode", "data": "esc"}])
    time.sleep(0.9)
    if n:
        keys(q, *(["down"] * n))
    keys(q, "ret")
    time.sleep(wait)


# Frames are compared in memory, stdlib only (Pillow is optional on this tree,
# which is why ppm2png.py exists at all). grab() keeps the raw RGB alongside
# the PNG so a scenario can assert on regions without decoding it again.
FRAMES = {}


def grab(q, tag):
    """screendump + convert (as shot()) and remember the raw RGB for diffing."""
    ppm = "shots/%s.ppm" % tag
    q.cmd("screendump", filename=ppm)
    time.sleep(0.4)
    w, h, rgb = ppm2png.read_ppm(ppm)
    ppm2png.write_png("shots/%s.png" % tag, w, h, rgb)
    os.remove(ppm)
    FRAMES[tag] = (w, h, rgb)
    print("shot: shots/%s.png" % tag)
    return w, h


def diff_frac(tag_a, tag_b, box):
    """Fraction of pixels differing between two frames inside box (l,t,r,b)."""
    wa, ha, ra = FRAMES[tag_a]
    wb, hb, rb = FRAMES[tag_b]
    if (wa, ha) != (wb, hb):
        return 1.0
    l, t, r, b = box
    l = max(0, l); t = max(0, t); r = min(wa, r); b = min(ha, b)
    if r <= l or b <= t:
        return 0.0
    n = 0
    for y in range(t, b):
        row = y * wa * 3
        for x in range(l, r):
            i = row + x * 3
            if (abs(ra[i] - rb[i]) + abs(ra[i + 1] - rb[i + 1]) +
                    abs(ra[i + 2] - rb[i + 2])) > 24:
                n += 1
    return n / float((r - l) * (b - t))


# diff_frac answers "did this region change"; a drag test also has to answer
# "WHERE is the window now", so these give the bounding box of what changed and
# the geometry helpers that go with it. Same rule: only ever between two frames
# of the same run.

def in_any(rects, x, y):
    for r in rects or ():
        if r[0] <= x < r[0] + r[2] and r[1] <= y < r[1] + r[3]:
            return True
    return False


def diff_bbox(tag_a, tag_b, thresh=24, ignore=None):
    """Bounding box (x, y, w, h) of the pixels that differ between two grabbed
    frames, or None. `ignore` is a list of rects that always change and would
    otherwise swallow the answer: the taskbar (its clock ticks) and, in a debug
    build, the perf HUD across the top."""
    wa, ha, ra = FRAMES[tag_a]
    wb, hb, rb = FRAMES[tag_b]
    if (wa, ha) != (wb, hb):
        return None
    xs0, ys0, xs1, ys1 = wa, ha, -1, -1
    for y in range(0, ha, 2):                     # every other row: 4x cheaper
        row = y * wa * 3
        for x in range(0, wa, 2):
            i = row + x * 3
            if (abs(ra[i] - rb[i]) + abs(ra[i + 1] - rb[i + 1]) +
                    abs(ra[i + 2] - rb[i + 2])) < thresh:
                continue
            if in_any(ignore, x, y):
                continue
            if x < xs0: xs0 = x
            if x > xs1: xs1 = x
            if y < ys0: ys0 = y
            if y > ys1: ys1 = y
    if xs1 < 0:
        return None
    return (xs0, ys0, xs1 - xs0 + 1, ys1 - ys0 + 1)


def uniform(tag, x, y, bw, bh, thresh=24):
    """Is every pixel of this box the same colour as its top-left? True over
    bare wallpaper (the gradient moves far less than `thresh` across a small
    box), false over an icon, a label or window chrome."""
    w, h, img = FRAMES[tag]
    if x < 0 or y < 0 or x + bw > w or y + bh > h:
        return False
    i0 = (y * w + x) * 3
    r0, g0, b0 = img[i0], img[i0 + 1], img[i0 + 2]
    for yy in range(y, y + bh, 2):
        row = yy * w * 3
        for xx in range(x, x + bw, 2):
            i = row + xx * 3
            if (abs(img[i] - r0) + abs(img[i + 1] - g0) +
                    abs(img[i + 2] - b0)) >= thresh:
                return False
    return True


def find_bare(tag, y0, y1, need=200):
    """The centre of the widest run of columns that is bare wallpaper across the
    whole band y0..y1: where a miss-test drag can press without touching a
    window or a desktop icon.

    A point merely "clear of the artwork" is not good enough - a desktop icon's
    clickable CELL is far wider than the glyph and label drawn in it, so a press
    in the blank part of a cell still launches the app. A run at least `need` px
    wide cannot be a gap between icons in one column, so it is past the grid."""
    w, h, _ = FRAMES[tag]
    nb = w // 8
    bare = [True] * nb
    for xb in range(nb):
        for y in range(y0, y1 - 8, 8):
            if not uniform(tag, xb * 8, y, 8, 8):
                bare[xb] = False
                break
    best, run = None, 0
    for xb in range(nb + 1):
        if xb < nb and bare[xb]:
            run += 1
            continue
        if run * 8 >= need and (best is None or run * 8 > best[1]):
            best = ((xb - run) * 8, run * 8)
        run = 0
    if not best:
        return None
    return best[0] + best[1] // 2, (y0 + y1) // 2


def win_title_y(desk_tag, open_tag, box, probe=90):
    """Offset from box[1] to a row that is really inside the title bar.

    A window's diff bbox starts at the top of its soft DROP SHADOW, several
    pixels above the frame, so "box[1] + a constant" aims the drag above the
    window and grabs the desktop instead - which is how a drag test silently
    turns into a no-op. Walk down the window's centre column to the first row
    that is both opaque (differs from the wallpaper) and stable (the same colour
    a few rows lower), i.e. the title bar's fill."""
    w, h, desk = FRAMES[desk_tag]
    _, _, win = FRAMES[open_tag]
    x = box[0] + box[2] // 2
    for dy in range(0, probe):
        y = box[1] + dy
        if y + 12 >= h:
            break
        i = (y * w + x) * 3
        j = ((y + 6) * w + x) * 3
        if (abs(win[i] - desk[i]) + abs(win[i + 1] - desk[i + 1]) +
                abs(win[i + 2] - desk[i + 2])) < 24:
            continue                            # still wallpaper
        if (win[i] == win[j] and win[i + 1] == win[j + 1] and
                win[i + 2] == win[j + 2]):
            return dy + 8                       # inside the bar, clear of its edge
    return 14


def noise_bands():
    """Rects that differ between any two frames whatever the windows did: the
    taskbar (clock) and, in a debug build, the perf HUD across the top."""
    return [(0, SCREEN_H - 64, SCREEN_W, 64), (0, 0, SCREEN_W, 40)]


def quiet_debug_cfg():
    """Disarm the fuzz driver for the duration of a UI scenario.

    `UNO_DEBUG=1 ./build.sh` stages a DEBUG.CFG onto the ESP, and the mere
    PRESENCE of that file arms pc64_stress.c, which opens and closes apps at
    random from the shell's own main loop. A window-management test cannot tell
    that apart from a bug in itself - the first run of wm_a had Studio launch
    mid-drag - so the scenario rewrites the file with `nostress` rather than
    hoping. The kernel log and the drag-frame cycle report are unaffected."""
    p = "build/esp/DEBUG.CFG"
    if os.path.exists(p):
        with open(p, "w") as f:
            f.write("nostress\nnoshutdown\nnonet\n")
        print("DEBUG.CFG: stress driver disarmed for this scenario")


class Mouse:
    """The machine's PS/2 mouse - the only pointer that can express a DRAG in
    QEMU, so build the image for it:

        UNO_DETACH=1 UNO_DEBUG=1 ./build.sh        (and pointer="none")

    Three pointer paths were tried before this one, and the first two cannot
    carry a drag at all:

    * firmware AbsolutePointer + usb-tablet (what mouse_move/click script, and
      what the M4 scenario has always used): this OVMF carries no USB pointer
      driver, so the guest never sees the tablet. Every scripted click in that
      scenario goes nowhere, silently - which is what alive() exists to catch.
    * native USB HID + usb-mouse (-DUNO_USBHID_TEST): enumerates, and the cursor
      tracks, but a HELD button does not survive. A boot-protocol mouse reports
      only on change, and uno_usb_hid_mouse_poll() returns 0 for the button on
      every frame with no report, so the shell sees press-release-press instead
      of a hold and the drag commits before it starts. The PS/2 path latches the
      button (`gMBtn`, pc64_native.c) and does not have this bug; a request to
      the usb lane to latch the USB one is filed in UNOAUTOMATE-REQUESTS.md.
    * PS/2, after detach: latched buttons, real drags. Used here.

    Deltas are guest framebuffer pixels 1:1 (poll_pointer adds them straight to
    the cursor), so this class tracks the cursor itself and works in SCREENDUMP
    coordinates, dividing by SCALE on the way out. Position is re-established by
    slamming into the top-left corner, which the guest clamps."""

    STEP = 100                                  # per event; the PS/2 packet is a signed byte

    def __init__(self, q):
        self.q = q
        self.x = self.y = 0
        self.home()

    def _rel(self, dx, dy):
        self.q.cmd("input-send-event", events=[
            {"type": "rel", "data": {"axis": "x", "value": dx}},
            {"type": "rel", "data": {"axis": "y", "value": dy}}])
        time.sleep(0.05)

    def home(self):
        n = max(SCREEN_W, SCREEN_H) // SCALE // self.STEP + 2
        for _ in range(n):
            self._rel(-self.STEP, -self.STEP)
        self.x = self.y = 0
        time.sleep(0.2)

    def to(self, sx, sy):
        """Move to a SCREENDUMP coordinate (absolute), in steps a packet can
        carry."""
        gx, gy = int(sx) // SCALE, int(sy) // SCALE
        while gx != self.x or gy != self.y:
            dx = max(-self.STEP, min(self.STEP, gx - self.x))
            dy = max(-self.STEP, min(self.STEP, gy - self.y))
            self._rel(dx, dy)
            self.x += dx
            self.y += dy
        time.sleep(0.1)

    def btn(self, down):
        self.q.cmd("input-send-event", events=[
            {"type": "btn", "data": {"down": down, "button": "left"}}])
        time.sleep(0.12)

    def park(self):
        """Sit in the ignored taskbar band, so the composited cursor is never
        itself the thing a diff finds."""
        self.to(SCREEN_W - 4, SCREEN_H - 4)
        time.sleep(0.3)

    def alive(self):
        """Prove the guest is really tracking this device before any scenario
        asserts on a drag."""
        self.home()
        time.sleep(0.4)
        grab(self.q, "_ptr_a")
        self.to(SCREEN_W // 2, SCREEN_H // 2)
        time.sleep(0.4)
        grab(self.q, "_ptr_b")
        moved = diff_bbox("_ptr_a", "_ptr_b", ignore=noise_bands()) is not None
        for t in ("_ptr_a", "_ptr_b"):
            os.remove("shots/%s.png" % t)
        return moved


REQUIRE_EDGE = os.environ.get("WM_REQUIRE_EDGE", "0") != "0"


def wm_d(q):
    """Gate D: Alt-Tab switcher, its Ctrl-Tab fallback, Alt+Left snap, Alt+D.

    Asserts by comparing regions between this run's own shots - never against
    a committed golden image, because themes and fonts drift.

    Run it TWICE to cover both modifier sources:
      UNO_DEBUG=1 ./build.sh                 && python3 harness.py wm_d
          the attached boot: modifiers come from the firmware Ex KeyState
          latch, which OVMF gives no release edge for, so the overlay commits
          on the stale-latch backstop.
      UNO_DEBUG=1 UNO_DETACH=1 ./build.sh    && WM_REQUIRE_EDGE=1 python3 harness.py wm_d
          the detached boot: firmware input is gone and the ONLY source of
          Alt make/break is the native PS/2 tracker, so the release edge is
          required. This is the run that proves pc64_native.c's tracker."""
    fails = []

    def check(name, ok, detail=""):
        print("  %-34s %s %s" % (name, "PASS" if ok else "FAIL", detail))
        if not ok:
            fails.append(name)

    # three more windows, so the MRU order has something to say
    start_app(q, 1)                       # Editor
    start_app(q, 2)                       # Files
    start_app(q, 4)                       # Clock
    W, H = grab(q, "wm_d_00_before")
    # the overlay is centred in the work area; sample a band across its middle
    band = (W // 4, H // 2 - 60, W - W // 4, H // 2 + 60)

    # ---- Alt HELD across two Tab steps -----------------------------------
    key_evt(q, "alt", True)
    time.sleep(0.25)
    tap(q, "tab")
    time.sleep(0.35)
    grab(q, "wm_d_01_overlay")
    check("alt-tab opens the overlay",
          diff_frac("wm_d_00_before", "wm_d_01_overlay", band) > 0.05)
    tap(q, "tab")
    time.sleep(0.35)
    grab(q, "wm_d_02_step2")
    check("second tab moves the selection",
          diff_frac("wm_d_01_overlay", "wm_d_02_step2", band) > 0.01)
    check("overlay survives a held Alt",
          diff_frac("wm_d_00_before", "wm_d_02_step2", band) > 0.05)

    # release: the commit must land on the modifier edge, well inside the
    # 5 s stale-latch backstop.  This is the assertion that proves the LIVE
    # modifier source saw Alt go up - on a detached boot that is the PS/2
    # make/break tracker, which nothing else in the suite exercises.
    key_evt(q, "alt", False)
    time.sleep(0.45)
    grab(q, "wm_d_03_committed")
    edge = diff_frac("wm_d_02_step2", "wm_d_03_committed", band) > 0.05
    if edge:
        check("alt release commits (modifier edge)", True)
        last = "wm_d_03_committed"
    else:
        # attached boots read modifiers from a per-keystroke firmware latch,
        # which has no release edge unless the firmware exposes partial
        # keystrokes; the overlay then commits on the stale-latch backstop.
        check("alt release commits (modifier edge)", not REQUIRE_EDGE,
              "(latched source, no release edge - set WM_REQUIRE_EDGE=1 on a "
              "detached build to demand it)")
        time.sleep(3.5)
        grab(q, "wm_d_03b_backstop")
        check("...commits on the stale-latch backstop",
              diff_frac("wm_d_02_step2", "wm_d_03b_backstop", band) > 0.05)
        last = "wm_d_03b_backstop"
    check("committing raises a different window",
          diff_frac("wm_d_00_before", last, band) > 0.005)

    # ---- Ctrl-Tab fallback: same overlay, commits on the ~0.8 s timer ----
    grab(q, "wm_d_04_pre_fallback")
    q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                            {"type": "qcode", "data": "tab"}])
    time.sleep(0.30)
    grab(q, "wm_d_05_fallback_overlay")
    check("ctrl-tab opens the same overlay",
          diff_frac("wm_d_04_pre_fallback", "wm_d_05_fallback_overlay", band) > 0.05)
    time.sleep(1.6)                       # past the ~0.8 s commit timer
    grab(q, "wm_d_06_fallback_done")
    check("ctrl-tab commits on the timer",
          diff_frac("wm_d_05_fallback_overlay", "wm_d_06_fallback_done", band) > 0.05)

    # ---- Alt+Left: snap the focused window to the left half --------------
    left = (2, H // 3, 24, 2 * H // 3)
    grab(q, "wm_d_07_pre_snap")
    key_evt(q, "alt", True)
    time.sleep(0.15)
    tap(q, "left")
    key_evt(q, "alt", False)
    time.sleep(0.8)
    grab(q, "wm_d_08_snap_left")
    check("alt+left fills the left edge",
          diff_frac("wm_d_07_pre_snap", "wm_d_08_snap_left", left) > 0.20)

    # ---- Alt+D: show desktop, then restore -------------------------------
    mid = (W // 6, H // 6, 5 * W // 6, 5 * H // 6)
    key_evt(q, "alt", True)
    time.sleep(0.15)
    tap(q, "d")
    key_evt(q, "alt", False)
    time.sleep(0.8)
    grab(q, "wm_d_09_showdesk")
    check("alt+d parks every window",
          diff_frac("wm_d_08_snap_left", "wm_d_09_showdesk", mid) > 0.20)
    key_evt(q, "alt", True)
    time.sleep(0.15)
    tap(q, "d")
    key_evt(q, "alt", False)
    time.sleep(0.8)
    grab(q, "wm_d_10_restored")
    check("alt+d again restores the same set",
          diff_frac("wm_d_09_showdesk", "wm_d_10_restored", mid) > 0.20)

    # the System window carries the detach verdict, which is what decides
    # WHICH modifier source the run above actually exercised
    start_app(q, 3, wait=2.0)
    grab(q, "wm_d_11_system")

    print("wm_d: %s" % ("PASS" if not fails else "FAIL " + ", ".join(fails)))
    return 0 if not fails else 1


def wm_a():
    """Gate A: a drag MOVES the window (no rubber band), a double-click on the
    title bar maximizes and restores, and the geometry survives a reboot.

    Needs a pointer that can hold a button down, so build for the PS/2 one:
        UNO_DETACH=1 UNO_DEBUG=1 ./build.sh && python3 harness.py wm_a
    (the Mouse class documents why nothing else in QEMU can drag). It owns its
    QEMU instances because the last assertion is about a POWER CYCLE."""
    fails = []

    def check(name, ok, detail=""):
        print("  %-44s %s %s" % (name, "PASS" if ok else "FAIL", detail))
        if not ok:
            fails.append(name)

    try:
        os.remove("build/esp/SHELL.CFG")       # deterministic first boot
    except OSError:
        pass
    quiet_debug_cfg()
    # A REAL FAT image, not the vvfat view. vvfat's read-write mode is not a
    # filesystem the guest can trust: SHELL.CFG written through it came back as
    # 50 bytes of unrelated garbage, which reads exactly like a shell bug and is
    # not one. mkuefi.py packs the same build/esp into a GPT + FAT32 image the
    # native driver writes properly, and the file survives the power cycle.
    subprocess.run([sys.executable, "tools/mkuefi.py"], check=True)
    os.environ["UNO_DISK"] = "build/unodos-uefi.img"
    dropbox = None
    qemu, q = start_qemu(log="build/wm_a1.log", pointer="none")
    try:
        print("wm_a: boot 1")
        time.sleep(18)
        probe_screen(q)
        band = noise_bands()
        m = Mouse(q)
        check("the guest has a pointer", m.alive(), "(needs UNO_DETACH=1)")
        if fails:
            raise SystemExit("wm_a: no pointer in the guest - nothing to drag")
        m.park()
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "w"}])
        time.sleep(1.5)                        # the restored Control Panel has
        grab(q, "wm_a_00_desktop")             # focus at boot: that closed it

        # ---- miss-test 1: press and drag on bare wallpaper with nothing open.
        # NOTHING may change. This is the check that catches a mis-scaled
        # pointer (it would land on a desktop icon and launch it); without it
        # every drag assertion below can pass by dragging thin air.
        bare = find_bare("wm_a_00_desktop", int(SCREEN_H * 0.15), SCREEN_H - 70)
        check("found bare wallpaper to miss with", bare is not None, str(bare))
        if bare:
            mx, my = bare
            m.to(mx, my); m.btn(True)
            for i in range(1, 8):
                m.to(mx, my + i * 16)
            m.btn(False)
            m.park()
            grab(q, "wm_a_01_miss_desktop")
            mb = diff_bbox("wm_a_00_desktop", "wm_a_01_miss_desktop", ignore=band)
            check("drag on bare desktop changed nothing", mb is None, str(mb))

        start_app(q, 1, wait=2.5)              # menu order = app order: Editor
        m.park()
        grab(q, "wm_a_02_editor_open")
        box = diff_bbox("wm_a_00_desktop", "wm_a_02_editor_open", ignore=band)
        check("Editor window opened",
              box is not None and box[2] > 100 and box[3] > 80, str(box))
        if box is None:
            raise SystemExit("wm_a: no Editor window - nothing to drag")
        title_in = win_title_y("wm_a_00_desktop", "wm_a_02_editor_open", box)

        # ---- miss-test 2: the same gesture a few rows LOWER, in the document
        # body, must not move the window. A pointer off by a title bar's height
        # would move it, and this is the only assertion that can tell "the drag
        # works" from "the drag is aimed too high".
        cx = box[0] + box[2] // 2
        m.to(cx, box[1] + title_in + 140); m.btn(True)
        for i in range(1, 8):
            m.to(cx + i * 25, box[1] + title_in + 140)
        m.btn(False)
        m.park()
        grab(q, "wm_a_03_miss_body")
        mb2 = diff_bbox("wm_a_00_desktop", "wm_a_03_miss_body", ignore=band)
        check("a drag in the window BODY did not move it",
              mb2 is not None and abs(mb2[0] - box[0]) <= 8 and
              abs(mb2[2] - box[2]) <= 8, str(mb2))

        # ---- the live drag ---------------------------------------------------
        tb_y = box[1] + title_in
        dx, steps = 200, 10
        m.to(cx, tb_y)
        m.btn(True)
        for i in range(1, steps // 2 + 1):
            m.to(cx + i * (dx // steps), tb_y)
        grab(q, "wm_a_04_mid_drag")            # button STILL DOWN
        midbox = diff_bbox("wm_a_00_desktop", "wm_a_04_mid_drag", ignore=band)
        for i in range(steps // 2 + 1, steps + 1):
            m.to(cx + i * (dx // steps), tb_y)
        m.btn(False)
        m.park()
        grab(q, "wm_a_05_dropped")
        dropbox = diff_bbox("wm_a_00_desktop", "wm_a_05_dropped", ignore=band)

        half = dx // 2
        check("window moved DURING the drag",
              midbox is not None and
              half * 0.6 <= midbox[0] - box[0] <= half * 1.4,
              "%s vs %s" % (midbox, box))
        # a rubber-band drag paints the window AND an outline offset from it, so
        # its changed region is much wider than one window
        check("mid-drag frame holds ONE window, no outline",
              midbox is not None and abs(midbox[2] - box[2]) <= 24, str(midbox))
        check("drop committed the move",
              dropbox is not None and dx * 0.85 <= dropbox[0] - box[0] <= dx * 1.15,
              str(dropbox))

        # ---- double-click the title bar: maximize, then restore -------------
        m.to(cx + dx, tb_y)
        for _ in range(2):
            m.btn(True); m.btn(False)
        time.sleep(0.8)
        m.park()
        grab(q, "wm_a_06_maximized")
        maxbox = diff_bbox("wm_a_00_desktop", "wm_a_06_maximized", ignore=band)
        check("double-click filled the work area",
              maxbox is not None and maxbox[2] >= SCREEN_W * 0.92 and
              maxbox[1] <= 44, str(maxbox))

        m.to(SCREEN_W // 2, title_in)          # the maximized bar sits at y=0
        for _ in range(2):
            m.btn(True); m.btn(False)
        time.sleep(0.8)
        m.park()
        grab(q, "wm_a_07_restored")
        rbox = diff_bbox("wm_a_00_desktop", "wm_a_07_restored", ignore=band)
        check("second double-click restored the rect",
              rbox is not None and dropbox is not None and
              abs(rbox[0] - dropbox[0]) <= 8 and abs(rbox[2] - dropbox[2]) <= 8,
              str(rbox))
        time.sleep(1.0)                        # let SHELL.CFG reach the disk
    finally:
        stop_qemu(qemu, q)

    # ---- reboot: the window comes back where it was dragged to --------------
    qemu, q = start_qemu(log="build/wm_a2.log", pointer="none")
    try:
        print("wm_a: boot 2 (session restore)")
        time.sleep(18)
        probe_screen(q)
        grab(q, "wm_a_08_after_reboot")
        abox = diff_bbox("wm_a_00_desktop", "wm_a_08_after_reboot",
                         ignore=noise_bands())
        check("reopened at the dragged position",
              abox is not None and dropbox is not None and
              abs(abox[0] - dropbox[0]) <= 8 and abs(abox[1] - dropbox[1]) <= 8,
              "%s vs %s" % (abox, dropbox))
    finally:
        stop_qemu(qemu, q)

    print("wm_a: %s" % ("PASS" if not fails else "FAIL " + ", ".join(fails)))
    return 0 if not fails else 1


# Where the generic title-bar buttons land, for the ACTIVE theme (Aurora Light:
# frame_w 1, title_h 26, minbox = maxbox = 13 - unoui/themes/theme_aurora.c)
# plus the painter's own 4 px right margin and 4 px gap (unoui.c,
# unoui_titlebtn_rect). Guest pixels, measured from the window's top-right
# corner, which is why every click below is aimed at a window whose right edge
# is the screen's: snapped right, or maximized. Deriving it instead of hunting
# for the box means a theme change breaks this test loudly rather than making
# it click empty title bar and "pass".
TB_FRAME, TB_TITLE, TB_BOX = 1, 26, 13


def titlebtn_xy(right, top, which):
    """Centre of the min ("min") or max ("max") box of a window whose top-right
    corner is (right, top), in guest pixels."""
    x = right - TB_FRAME - 4 - TB_BOX // 2
    if which == "min":
        x -= TB_BOX + 4
    return x, top + TB_FRAME + (TB_TITLE - TB_FRAME - TB_BOX) // 2 + TB_BOX // 2


def wm_b():
    """Gate B: the title-bar minimize box, the taskbar chip toggle, the maximize
    box, and the parked set surviving a power cycle.

        UNO_DETACH=1 UNO_DEBUG=1 ./build.sh && python3 harness.py wm_b

    Same two requirements as wm_a, for the same reasons: the PS/2 pointer (the
    only one the guest sees, see the Mouse class) and a real FAT image (vvfat
    hands SHELL.CFG back as garbage). It owns its QEMU instances because the
    last assertion is about a REBOOT."""
    fails = []

    def check(name, ok, detail=""):
        print("  %-46s %s %s" % (name, "PASS" if ok else "FAIL", detail))
        if not ok:
            fails.append(name)

    try:
        os.remove("build/esp/SHELL.CFG")       # deterministic first boot
    except OSError:
        pass
    quiet_debug_cfg()
    subprocess.run([sys.executable, "tools/mkuefi.py"], check=True)
    os.environ["UNO_DISK"] = "build/unodos-uefi.img"
    qemu, q = start_qemu(log="build/wm_b1.log", pointer="none")
    try:
        print("wm_b: boot 1")
        time.sleep(18)
        probe_screen(q)
        band = noise_bands()
        gw = SCREEN_W // SCALE
        # the chip strip: the taskbar minus the tray, whose clock always ticks
        chip_only = [(0, 0, SCREEN_W, SCREEN_H - 64),
                     (SCREEN_W - 260, SCREEN_H - 64, 260, 64)]
        m = Mouse(q)
        check("the guest has a pointer", m.alive(), "(needs UNO_DETACH=1)")
        if fails:
            raise SystemExit("wm_b: no pointer in the guest - nothing to click")
        m.park()
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "w"}])
        time.sleep(1.5)                        # close the restored Control Panel
        grab(q, "wm_b_00_desktop")

        start_app(q, 1, wait=2.5)              # menu order = app order: Editor
        m.park()
        grab(q, "wm_b_01_open")
        box = diff_bbox("wm_b_00_desktop", "wm_b_01_open", ignore=band)
        check("Editor window opened",
              box is not None and box[2] > 100 and box[3] > 80, str(box))
        if box is None:
            raise SystemExit("wm_b: no Editor window - nothing to click")

        # Snap it right (Alt+Right, phase D) so its top-right corner is the
        # screen's: from here the button geometry is exact, not guessed.
        key_evt(q, "alt", True); time.sleep(0.2)
        tap(q, "right")
        time.sleep(0.3)
        key_evt(q, "alt", False); time.sleep(1.0)
        m.park()
        grab(q, "wm_b_02_snapped")
        sbox = diff_bbox("wm_b_00_desktop", "wm_b_02_snapped", ignore=band)
        # the top edge reads as 44, not 0: noise_bands() ignores the debug HUD
        # across the top 40 px, so no bbox can begin above it
        check("snapped right (top-right corner = screen's)",
              sbox is not None and sbox[0] > SCREEN_W * 0.4 and
              sbox[1] <= 44 and sbox[0] + sbox[2] >= SCREEN_W - 8, str(sbox))

        # ---- the maximize BOX: a real click on the chrome ------------------
        mxx, mxy = titlebtn_xy(gw, 0, "max")
        m.to(mxx * SCALE, mxy * SCALE); m.btn(True); m.btn(False)
        time.sleep(0.8)
        m.park()
        grab(q, "wm_b_03_max")
        maxbox = diff_bbox("wm_b_00_desktop", "wm_b_03_max", ignore=band)
        check("clicking the maximize box filled the work area",
              maxbox is not None and maxbox[2] >= SCREEN_W * 0.92 and
              maxbox[1] <= 44, str(maxbox))

        # ---- the minimize BOX ---------------------------------------------
        mnx, mny = titlebtn_xy(gw, 0, "min")
        m.to(mnx * SCALE, mny * SCALE); m.btn(True); m.btn(False)
        time.sleep(0.8)
        m.park()
        grab(q, "wm_b_04_parked")
        gone = diff_bbox("wm_b_00_desktop", "wm_b_04_parked", ignore=band)
        check("clicking the minimize box took the window off screen",
              gone is None, str(gone))
        # Locate the chip against the EMPTY desktop, not against a frame that
        # still has the window in it: a window's soft drop shadow reaches a few
        # rows into the taskbar band, and that bleed swallows the bbox and puts
        # its centre on bare bar between chips - a click that lands on nothing
        # and reads as "restore is broken".
        chip = diff_bbox("wm_b_00_desktop", "wm_b_04_parked", ignore=chip_only)
        check("its taskbar chip is still drawn, changed to parked",
              chip is not None and 40 < chip[2] < 400, str(chip))
        if chip is None:
            raise SystemExit("wm_b: no chip to click")
        cx, cy = chip[0] + chip[2] // 2, chip[1] + chip[3] // 2

        # ---- the chip toggle: parked -> restore, focused -> park -----------
        m.to(cx, cy); m.btn(True); m.btn(False)
        time.sleep(1.0)
        m.park()
        grab(q, "wm_b_05_chip_restored")
        rbox = diff_bbox("wm_b_00_desktop", "wm_b_05_chip_restored", ignore=band)
        check("the chip restored it, still maximized",
              rbox is not None and rbox[2] >= SCREEN_W * 0.92, str(rbox))

        m.to(cx, cy); m.btn(True); m.btn(False)  # it has focus now: park it
        time.sleep(1.0)
        m.park()
        grab(q, "wm_b_06_chip_parked")
        gone2 = diff_bbox("wm_b_00_desktop", "wm_b_06_chip_parked", ignore=band)
        check("clicking the FOCUSED app's chip parks it", gone2 is None, str(gone2))

        m.to(cx, cy); m.btn(True); m.btn(False)  # and back again
        time.sleep(1.0)
        m.park()
        grab(q, "wm_b_07_chip_back")

        # ---- the maximize box again: this time it must RESTORE -------------
        m.to(mxx * SCALE, mxy * SCALE); m.btn(True); m.btn(False)
        time.sleep(0.8)
        m.park()
        grab(q, "wm_b_08_unmax")
        ubox = diff_bbox("wm_b_00_desktop", "wm_b_08_unmax", ignore=band)
        check("a second click on the maximize box restored the rect",
              ubox is not None and ubox[2] < SCREEN_W * 0.92, str(ubox))

        # ---- Ctrl-M, the keyboard twin of the minimize box. It also leaves
        # the Editor parked for the reboot assertion, and gets it out of the
        # frame: with two windows open a diff bbox is their UNION, and Paint
        # merely MOVING below would change it. -----------------------------
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "m"}])
        time.sleep(1.5)
        m.park()
        grab(q, "wm_b_09_ctrlm_parked")
        check("ctrl-m parks the focused window",
              diff_bbox("wm_b_00_desktop", "wm_b_09_ctrlm_parked",
                        ignore=band) is None)

        # ---- Paint carries no UI_WIN_RESIZE, so its maxbox is painted
        # disabled and on_action drops UI_ACT_MAX outright. Driven from the
        # keyboard here: Alt+Up reaches the same handler, and unlike the click
        # it needs no window geometry. It must never resize the window. ------
        start_app(q, 12, wait=2.5)             # Paint
        m.park()
        grab(q, "wm_b_10_paint")
        pbox = diff_bbox("wm_b_00_desktop", "wm_b_10_paint", ignore=band)
        key_evt(q, "alt", True); time.sleep(0.2)
        tap(q, "up")
        time.sleep(0.3)
        key_evt(q, "alt", False); time.sleep(0.8)
        m.park()
        grab(q, "wm_b_11_paint_after")
        pbox2 = diff_bbox("wm_b_00_desktop", "wm_b_11_paint_after", ignore=band)
        check("a non-resizable window is never resized by maximize",
              pbox is not None and pbox2 is not None and
              abs(pbox2[2] - pbox[2]) <= 8 and abs(pbox2[3] - pbox[3]) <= 8,
              "%s vs %s" % (pbox2, pbox))

        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "w"}])
        time.sleep(1.5)                        # close Paint: only the parked
        m.park()                               # Editor is left in the session
        grab(q, "wm_b_12_only_parked")
        check("closing Paint leaves just the parked Editor",
              diff_bbox("wm_b_00_desktop", "wm_b_12_only_parked",
                        ignore=band) is None)
        time.sleep(1.0)                        # let SHELL.CFG reach the disk
    finally:
        stop_qemu(qemu, q)

    # ---- reboot: minN= brings it back OPEN and PARKED ----------------------
    qemu, q = start_qemu(log="build/wm_b2.log", pointer="none")
    try:
        print("wm_b: boot 2 (session restore)")
        time.sleep(18)
        probe_screen(q)
        grab(q, "wm_b_13_after_reboot")
        body = diff_bbox("wm_b_00_desktop", "wm_b_13_after_reboot",
                         ignore=noise_bands())
        check("restored session shows no window (still parked)",
              body is None, str(body))
        back = diff_bbox("wm_b_00_desktop", "wm_b_13_after_reboot",
                         ignore=chip_only)
        check("but its chip is back on the taskbar",
              back is not None and 40 < back[2] < 400, str(back))
    finally:
        stop_qemu(qemu, q)

    print("wm_b: %s" % ("PASS" if not fails else "FAIL " + ", ".join(fails)))
    return 0 if not fails else 1


def qemu_argv(extra=None, log="build/ovmf.log", pointer="tablet"):
    # Storage: a vvfat view of build/esp by default (no image build needed).
    # vvfat's read-write mode corrupts multi-cluster WRITES, though, so tests
    # that write on-device (Studio build-and-run of a larger app) set UNO_DISK
    # to a real FAT image built by tools/mkuefi.py, where writes are reliable.
    disk = os.environ.get("UNO_DISK")
    disk_arg = ("format=raw,file=" + disk) if disk else "format=vvfat,file=fat:rw:build/esp"
    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", disk_arg,
        "-device", "qemu-xhci",
        "-nic", "none",
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-debugcon", "file:" + log, "-global", "isa-debugcon.iobase=0x402",
    ]
    # AT MOST ONE pointer device: QEMU routes an input event to whichever
    # handler registered first, so two pointers means the motion and the clicks
    # can land on different ones. "none" leaves only the machine's built-in
    # PS/2 mouse, which is what the Mouse class drives.
    if pointer != "none":
        argv += ["-device", "usb-mouse" if pointer == "mouse" else "usb-tablet"]
    return argv + (extra or [])


def start_qemu(extra=None, log="build/ovmf.log", pointer="tablet"):
    """Boot one QEMU and connect QMP. Returns (proc, Qmp). A scenario that needs
    a REBOOT (session restore) calls this twice; build/esp is the same vvfat
    tree both times, so what the guest wrote survives the power cycle."""
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    qemu = subprocess.Popen(qemu_argv(extra, log, pointer))
    return qemu, Qmp(QMP_SOCK)


def stop_qemu(qemu, q):
    try:
        q.cmd("quit")
    except Exception:
        qemu.kill()
    qemu.wait(timeout=10)


def main():
    rc = [0]
    if len(sys.argv) > 1 and sys.argv[1] == "wm_a":
        return wm_a()                          # owns its own two QEMU boots
    if len(sys.argv) > 1 and sys.argv[1] == "wm_b":
        return wm_b()                          # ditto: it asserts on a reboot
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    argv = qemu_argv()
    if len(sys.argv) > 1 and sys.argv[1] == "acpi":
        # unoacpi verification: inject the synthetic battery/lid SSDT (50%).
        # Build it first if needed:  iasl tools/testbat.asl
        argv += ["-acpitable", "file=tools/testbat.aml"]
    if len(sys.argv) > 1 and sys.argv[1] == "lidsleep":
        # lid-close sleep: inject the TOGGLING-lid SSDT (open ~5s / closed ~5s).
        # Build it first if needed:  iasl tools/testlid.asl
        argv += ["-acpitable", "file=tools/testlid.aml"]
    qemu = subprocess.Popen(argv)
    try:
        q = Qmp(QMP_SOCK)
        print("qemu up; waiting for OVMF -> UnoDOS boot...")
        time.sleep(18)                        # OVMF + splash + first desktop paint
        probe_screen(q)                       # mouse coords need the real mode
        shot(q, "m1_desktop")

        if len(sys.argv) > 1 and sys.argv[1] == "boot":
            return

        # ---- window-manager gates (docs/WM-MODERN-SPEC.md) ----------------
        if len(sys.argv) > 1 and sys.argv[1] == "wm_d":
            rc[0] = wm_d(q)
            return

        # ---- Duum: the Python Doom engine.  Studio greets the Duum source
        # (staged as SAMPLE.PY by the runner), Ctrl-B packs it, Ctrl-R runs it;
        # it streams DOOM1.WAD and renders E1M1.  Generous waits: the first BSP
        # render is heavy in the interpreter.  Move forward, shoot a second
        # frame to prove the view changes. ------------------------------------
        if len(sys.argv) > 1 and sys.argv[1] == "duum":
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "esc"}])
            time.sleep(0.8)
            keys(q, *(["down"] * 15)); keys(q, "ret")   # Studio
            time.sleep(2.5)
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "b"}])
            time.sleep(2.5)
            shot(q, "duum_build")
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "r"}])
            time.sleep(28)                              # first render (+texture compose)
            shot(q, "duum_a")
            for _ in range(4):                          # walk forward
                keys(q, "up"); time.sleep(5)
            shot(q, "duum_b")
            keys(q, "left"); time.sleep(6)             # turn
            shot(q, "duum_c")
            return

        # ---- Python: Studio greets SDK\SAMPLE.PY; Ctrl-B packs it into a
        # UNO_MODF_PYAPP container, Ctrl-R hands it to PYRT.UNO which compiles
        # and runs it.  Two shots a beat apart prove the ball animates. -------
        if len(sys.argv) > 1 and sys.argv[1] == "py":
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "esc"}])
            time.sleep(0.8)
            keys(q, *(["down"] * 15)); keys(q, "ret")   # last app = Studio
            time.sleep(2.5)
            shot(q, "py_studio")                         # editor with SAMPLE.PY
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "b"}])
            time.sleep(2.0)
            shot(q, "py_build")                          # "Packed SAMPLE.UNO"
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "r"}])
            time.sleep(2.0)
            shot(q, "py_run_a")                          # the Python app running
            time.sleep(1.3)
            shot(q, "py_run_b")                          # a later frame (ball moved)
            return

        # ---- lid-close sleep: the toggling-lid SSDT closes the lid ~5s after
        # boot (screen blanks = asleep) and reopens ~5s later (desktop wakes).
        # Sample across a full cycle. --------------------------------------
        if len(sys.argv) > 1 and sys.argv[1] == "lidsleep":
            for i in range(12):
                time.sleep(2)
                shot(q, "lid_%02d" % i)
            return

        # ---- unoacpi: synthetic battery boot (harness.py acpi) -------------
        # Open System via the Start menu, keyboard-only: Ctrl-Esc opens the
        # launcher, Down x3 = System (menu order = app order), Enter activates.
        if len(sys.argv) > 1 and sys.argv[1] == "acpi":
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "esc"}])
            time.sleep(0.8)
            keys(q, "down", "down", "down"); keys(q, "ret")
            time.sleep(1.5)
            shot(q, "acpi_system")             # "ACPI AML: up ... bat 50% lid open"
            return

        # ---- M2 (decoupling): every bridge app is a .UNO module loaded from
        # storage; open each through the launcher (menu order = app order,
        # bridge apps at indices 7..13) and screenshot it running. ----------
        if len(sys.argv) > 1 and sys.argv[1] == "unoapps":
            bridge = ["dostris", "pacman", "outlast", "music",
                      "tracker", "paint", "network"]
            for i, name in enumerate(bridge):
                q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                        {"type": "qcode", "data": "esc"}])
                time.sleep(0.8)
                keys(q, *(["down"] * (7 + i)))
                keys(q, "ret")
                time.sleep(2.0)
                shot(q, "uno_" + name)
                q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                        {"type": "qcode", "data": "w"}])
                time.sleep(0.8)
            return

        # ---- Studio: the IDE loads from APPS\STUDIO.UNO, edits + builds +
        # runs a UnoC app on-device.  Launcher order = app order; with the IDE
        # shipped, Studio is the last listed app (after Runner + Browser). ----
        if len(sys.argv) > 1 and sys.argv[1] == "ide":
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "esc"}])
            time.sleep(0.8)
            keys(q, *(["down"] * 15))          # 0..14 apps, 15 = Studio
            keys(q, "ret")
            time.sleep(2.5)
            shot(q, "ide_open")                # editor with SAMPLE.C loaded
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "b"}])
            time.sleep(2.5)
            shot(q, "ide_build")               # SAMPLE.UNO written
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                    {"type": "qcode", "data": "r"}])
            time.sleep(2.5)
            shot(q, "ide_run")                 # the built app runs in its window
            return

        # ---- M4: mouse (usb-tablet -> EFI Absolute Pointer) ----------------
        if len(sys.argv) > 1 and sys.argv[1] == "mouse":
            click(q, 232, 50)                  # select the Files icon
            time.sleep(0.5)
            click(q, 232, 50)                  # second click = double: launch
            time.sleep(1.5)
            shot(q, "m4_mouse_open")
            mouse_move(q, 180, 48)             # grab the Files title bar
            mouse_btn(q, True)
            for step in range(1, 7):           # outline drag, WIn3.1 style
                mouse_move(q, 180 + step * 30, 48 + step * 25)
            mouse_btn(q, False)
            time.sleep(1.0)
            shot(q, "m4_mouse_drag")
            click(q, 500, 300)                 # background click
            time.sleep(0.5)
            shot(q, "m4_mouse_done")
            return

        # ---- M2: keyboard navigation. Desktop icon 0 = SysInfo. ----------
        keys(q, "ret"); time.sleep(1.5)
        shot(q, "m2_sysinfo")
        keys(q, "esc"); time.sleep(1.0)

        keys(q, "right"); keys(q, "ret"); time.sleep(1.5)      # icon 1 = Clock
        shot(q, "m2_clock")
        keys(q, "esc"); time.sleep(1.0)

        # ---- M2: Files (RAM disk listing: README.TXT) ---------------------
        keys(q, "right"); keys(q, "ret"); time.sleep(1.5)      # icon 2 = Files
        shot(q, "m2_files")
        keys(q, "esc"); time.sleep(1.0)

        # ---- M2: Notepad + typing + Ctrl-S (cmdKey) save -------------------
        keys(q, "right"); keys(q, "ret"); time.sleep(1.5)      # icon 3 = Notepad
        text(q, "UnoDOS on a modern PC via UEFI GOP.")
        time.sleep(1.0)
        shot(q, "m2_notepad")
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "s"}])
        time.sleep(1.0)
        keys(q, "esc"); time.sleep(1.0)

        # ---- M2: Files again - the saved file on the FAT volume ------------
        keys(q, "left"); keys(q, "ret"); time.sleep(1.5)       # icon 2 = Files
        keys(q, "r"); time.sleep(1.0)                          # refresh listing
        shot(q, "m2_files_saved")
        keys(q, "esc"); time.sleep(1.0)

        # ---- M3: Dostris ---------------------------------------------------
        keys(q, "right", "right", "right"); keys(q, "ret"); time.sleep(1.5)  # icon 5
        keys(q, "n"); time.sleep(0.5)          # new game
        for _ in range(6):
            keys(q, "left", gap=0.2)
            keys(q, "spc", gap=0.3)            # hard-drop a few pieces
        time.sleep(0.8)
        shot(q, "m3_dostris")
    finally:
        try:
            q.cmd("quit")
        except Exception:
            qemu.kill()
        qemu.wait(timeout=10)
    return rc[0]


if __name__ == "__main__":
    sys.exit(main() or 0)

#!/usr/bin/env python3
"""duum_urc - prove Duum runs as a game on the OS, not just on the host shim.

    UNO_DEBUG=1 ./build.sh && python3 tools/duum_urc.py

Boots the DEBUG image in QEMU, opens Duum by name, and walks the E1M1 start:
first frame renders (textured, HUD at the bottom), turning changes the view,
firing spends ammo (the HUD ammo digits repaint).  Driven over URC because
QEMU's usb-tablet delivers no pointer motion to this guest (tools/urcui.py);
Duum is keyboard-only anyway.

The first frame is the slow one (the interpreter parses an 11MB WAD directory
and composes every visible texture), so the launch settle is generous.
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from urcui import UrcUi, SHOTS                              # noqa: E402

fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


def grab(ui):
    w, h, rgba = ui.link.screen_grab(1, timeout=60)
    return w, h, rgba


def region_signature(rgba, w, x0, y0, x1, y1):
    """Cheap content hash of a screen region."""
    s = 0
    for y in range(y0, y1, 3):
        base = (y * w + x0) * 4
        for x in range(x0, x1, 7):
            i = base + (x - x0) * 4
            s = (s * 131 + rgba[i] + rgba[i + 1] * 7 + rgba[i + 2] * 29) & 0xFFFFFFFF
    return s


def nonblack_frac(rgba, w, x0, y0, x1, y1):
    n = 0; tot = 0
    for y in range(y0, y1, 4):
        for x in range(x0, x1, 4):
            i = (y * w + x) * 4
            tot += 1
            if rgba[i] or rgba[i + 1] or rgba[i + 2]:
                n += 1
    return n / max(1, tot)


with UrcUi() as ui:
    w, h = ui.size()
    print("screen %dx%d" % (w, h))
    ui.shot("duum_desktop")

    # the shell restores the previous session's windows; clear them so Duum
    # gets focus and the screen
    for t in list(ui.windows()):
        ui.link.command("close", timeout=10)
        time.sleep(0.5)

    # DUUM.UNO is a PYAPP - no descriptor, so it is not an app-registry row.
    # Launch it the way Files does: pc64_shell_run_user, via uno.run_app on
    # the URC `py` verb.  The .UNO lives on whichever FAT volume is the ESP.
    vol = None
    for v in range(4):
        r = ui.link.command("py", "import uno; print(uno.size(%d, 'APPS/DUUM.UNO'))" % v,
                            timeout=20)
        if r and r[0].strip().lstrip("-").isdigit() and int(r[0]) > 0:
            vol = v
            break
    print("DUUM.UNO on volume", vol)
    check(vol is not None, "DUUM.UNO is on a volume")
    r = ui.link.command("py", "import uno; print(uno.run_app(%d, 'APPS/DUUM.UNO'))" % vol,
                        timeout=30)
    print("run_app ->", r)
    time.sleep(6.0)
    titles = ui.windows()
    print("windows:", titles)
    check(any("duum" in t.lower() for t in titles), "a Duum window opened")

    # first frame: WAD parse + texture composition, generously waited
    time.sleep(25.0)
    ui.shot("duum_first_frame")
    gw, gh, rgba = grab(ui)

    # the canvas: middle of the screen; the HUD: its bottom band
    check(nonblack_frac(rgba, gw, gw // 4, gh // 4, 3 * gw // 4, gh // 2) > 0.9,
          "the 3D view rendered (not black)")
    sig_before = region_signature(rgba, gw, gw // 4, gh // 4, 3 * gw // 4, 3 * gh // 4)
    # the ammo counter lives INSIDE Duum's window (it is a desktop app, not
    # fullscreen): sample the window's lower-left, where the big digits are
    hud_before = region_signature(rgba, gw, 60, 250, 200, 308)

    # turn left ~90 degrees: view must change
    for _ in range(8):
        ui.key(0, 4, settle=0.25)
    time.sleep(2.0)
    ui.shot("duum_turned")
    gw, gh, rgba = grab(ui)
    sig_after = region_signature(rgba, gw, gw // 4, gh // 4, 3 * gw // 4, 3 * gh // 4)
    check(sig_after != sig_before, "turning changed the rendered view")

    # fire twice: the HUD ammo counter must repaint
    ui.key(ord('f'), 0, settle=0.4)
    ui.key(ord('f'), 0, settle=0.4)
    time.sleep(2.0)
    ui.shot("duum_fired")
    gw, gh, rgba = grab(ui)
    hud_after = region_signature(rgba, gw, 60, 250, 200, 308)
    check(hud_after != hud_before, "firing changed the HUD (ammo digits)")

    # walk forward a few steps and confirm the view keeps changing
    for _ in range(6):
        ui.key(0, 1, settle=0.25)
    time.sleep(2.0)
    ui.shot("duum_walked")
    gw, gh, rgba = grab(ui)
    sig_walk = region_signature(rgba, gw, gw // 4, gh // 4, 3 * gw // 4, 3 * gh // 4)
    check(sig_walk != sig_after, "walking changed the rendered view")

print()
if fails:
    print("DUUM URC: %d FAILURE(S)" % len(fails))
    for f in fails:
        print("  - " + f)
    sys.exit(1)
print("DUUM URC: PASS (shots in pc64/shots/)")

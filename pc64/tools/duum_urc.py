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

    idx = ui.launch_named("Duum", settle=6.0, tries=40)
    check(idx >= 0, "a window titled 'Duum' opened")

    # first frame: WAD parse + texture composition, generously waited
    time.sleep(25.0)
    ui.shot("duum_first_frame")
    gw, gh, rgba = grab(ui)

    # the canvas: middle of the screen; the HUD: its bottom band
    check(nonblack_frac(rgba, gw, gw // 4, gh // 4, 3 * gw // 4, gh // 2) > 0.9,
          "the 3D view rendered (not black)")
    sig_before = region_signature(rgba, gw, gw // 4, gh // 4, 3 * gw // 4, 3 * gh // 4)
    hud_before = region_signature(rgba, gw, 0, gh - 30, gw // 2, gh - 4)

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
    hud_after = region_signature(rgba, gw, 0, gh - 30, gw // 2, gh - 4)
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

#!/usr/bin/env python3
"""Prove the window-manager snap ANIMATES in QEMU, rather than teleporting.

Catching a 130 ms animation over URC is not something the request/response
screenshot path can do - one round trip is longer than the whole move. So this
uses the server-side recorder (`screen record`), which captures on the guest's
own shell tick into a RAM ring at up to 60 fps and is read back afterwards.
That is the instrument the remote-desktop lane already built for exactly this
shape of problem.

THE MEASUREMENT is one horizontal scanline, chosen below the title bars and
above the taskbar so nothing else on it changes: not the debug HUD (top), not
the tray clock (bottom). Per recorded frame, count how many pixels on that line
differ from the SETTLED frame. A window that teleports gives that count exactly
two values - "before" and "after". One that animates gives a descending run of
intermediates, and that run is what this asserts.

    UNO_DEBUG=1 ./build.sh && python3 tools/snap_anim_qemu.py [--explore]

--explore just launches the app and screenshots it, for re-reading the maximize
box's position off the picture when the layout moves.
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from urcui import UrcUi

FILES_SLOT = 2                 # kAppNames in pc64_uui.c: "Files"

# READ OFF shots/snap_explore.png, never computed. The title bar's two control
# boxes sit at x 568..580 (minimize) and 584..597 (maximize) on a 640x400
# desktop with the default theme; this is the centre of the right-hand one.
MAXBOX = (590, 66)

SCANLINE = 220                 # clear of the title bars, the HUD and the tray
MIN_INTERMEDIATE = 2           # distinct in-between states demanded
FPS = 60


def explore():
    with UrcUi() as ui:
        ui.link.command("launch", FILES_SLOT, timeout=15)
        time.sleep(3.0)
        print("windows:", ui.windows())
        print("screen :", ui.size())
        ui.shot("snap_explore")
    return 0


def scanline(frame, y):
    w, h, rgba = frame
    if y >= h:
        y = h // 2
    off = y * w * 4
    return rgba[off:off + w * 4]


def differing(a, b):
    """How many PIXELS differ between two scanlines."""
    n = min(len(a), len(b)) // 4
    return sum(1 for i in range(n) if a[i * 4:i * 4 + 3] != b[i * 4:i * 4 + 3])


def main():
    if "--explore" in sys.argv:
        return explore()

    with UrcUi() as ui:
        ui.link.command("launch", FILES_SLOT, timeout=15)
        time.sleep(3.0)
        if not any(t.startswith("Files") for t in ui.windows()):
            print("FAIL: slot %d did not open Files" % FILES_SLOT)
            return 1

        st0 = ui.link.screen_record_start(1, FPS, timeout=10)
        if st0.get("on") != 1:
            print("FAIL: the recorder did not start")
            return 1
        time.sleep(0.4)                       # a few settled frames first
        ui.click(*MAXBOX)                     # maximize
        time.sleep(1.0)
        st = ui.link.screen_record_stop(timeout=10)
        frames = ui.link.screen_record_frames(st, timeout=60)
        ui.shot("snap_maximized")

    print("recorded %d frames at %d fps" % (len(frames), st.get("fps", -1)))
    if len(frames) < 8:
        print("FAIL: too few frames recorded to say anything")
        return 1

    settled = scanline(frames[-1], SCANLINE)
    series = [differing(scanline(f, SCANLINE), settled) for f in frames]
    print("pixels differing from the settled frame, per recorded frame:")
    print("  " + " ".join(str(v) for v in series))

    # The window has to have MOVED at all: some frame must differ from the last.
    if max(series) == 0:
        print("FAIL: nothing on the scanline ever changed - the click missed, "
              "or the window never maximized")
        return 1

    # A teleport visits exactly two values. Anything strictly between the
    # extremes is a frame the window was caught in flight.
    lo, hi = min(series), max(series)
    mids = sorted({v for v in series if lo < v < hi})
    print("distinct values: %d (min %d, max %d); intermediates: %s"
          % (len(set(series)), lo, hi, mids if mids else "none"))
    if len(mids) < MIN_INTERMEDIATE:
        print("FAIL: the snap teleported - %d intermediate state(s), wanted %d. "
              "Either the animator is not installed or unoui_snap_ms is 0."
              % (len(mids), MIN_INTERMEDIATE))
        return 1
    if series[-1] != 0:
        print("FAIL: the last frame is not the settled one")
        return 1

    # The move must be one-way. Two sets of tweens writing the same rect - what
    # re-aiming a window mid-flight would do if it stacked instead of
    # cancelling - each win on alternate frames, and that reads as a window that
    # shakes. Here it would show up as the count going back UP.
    for i in range(1, len(series)):
        if series[i] > series[i - 1]:
            print("FAIL: the window moved BACKWARDS at frame %d (%d -> %d) - "
                  "something is fighting over its rect"
                  % (i, series[i - 1], series[i]))
            return 1

    print("PASS: the snap was caught in flight across %d intermediate states, "
          "moving one way" % len(mids))
    return 0


if __name__ == "__main__":
    sys.exit(main())

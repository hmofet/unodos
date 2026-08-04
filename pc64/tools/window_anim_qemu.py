#!/usr/bin/env python3
"""Prove a window RISES when it opens and leaves a collapsing ghost when it
closes, in QEMU.

Same instrument as the snap, launcher and switcher gates: the guest-side
`screen record` ring, because both motions are shorter than one URC screenshot
round trip. One recording covers both halves - launch an app, then close it.

OPEN is measured as the window's top edge on a column that crosses it: the
topmost pixel differing from the desktop before it opened. It should climb
through a run of positions as the window rises WIN_RISE px into place.

CLOSE cannot be measured that way, because by then the window is gone - the
teardown and removal are unchanged, and what animates is a ghost frame drawn
where it was. So the close half counts pixels differing from the SETTLED
post-close desktop inside a band clear of the taskbar and the debug HUD: the
ghost makes that non-zero for a few frames and then it goes to zero.

    UNO_DEBUG=1 ./build.sh && python3 tools/window_anim_qemu.py
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from urcui import UrcUi

FILES_SLOT = 2
COLUMN = 300                   # crosses the window that is about to open
TOP_SKIP = 30                  # below the debug HUD, which changes every frame
BAND = (40, 300)               # y range for the ghost: no taskbar, no HUD
MIN_INTERMEDIATE = 2
FPS = 60


def col_px(frame, x, y0, y1):
    w, h, rgba = frame
    y1 = min(y1, h)
    return [bytes(rgba[(y * w + x) * 4:(y * w + x) * 4 + 3]) for y in range(y0, y1)]


def top_edge(frame, base):
    for i, (p, q) in enumerate(zip(frame, base)):
        if p != q:
            return TOP_SKIP + i
    return None


def band_diff(a, b, w, y0, y1):
    n = 0
    for y in range(y0, y1):
        o = y * w * 4
        for x in range(0, w):
            i = o + x * 4
            if a[i:i + 3] != b[i:i + 3]:
                n += 1
    return n


def main():
    with UrcUi() as ui:
        # Clear the desktop first. With another window up, opening one
        # DEACTIVATES it - its title bar changes colour - and that difference
        # sits above the new window, so "the topmost changed pixel" reported the
        # other window's title bar and never moved. Against a bare desktop the
        # only thing that changes on the column is the window being measured.
        for _ in range(4):
            if not ui.windows():
                break
            ui.link.command("close", timeout=10)
            time.sleep(0.8)
        if ui.windows():
            print("FAIL: could not clear the desktop, still %r" % (ui.windows(),))
            return 1

        st0 = ui.link.screen_record_start(1, FPS, timeout=10)
        if st0.get("on") != 1:
            print("FAIL: the recorder did not start")
            return 1
        time.sleep(0.30)
        ui.link.command("launch", FILES_SLOT, timeout=15)      # rise
        time.sleep(1.20)
        ui.link.command("close", timeout=10)                   # ghost
        time.sleep(0.60)
        st = ui.link.screen_record_stop(timeout=10)
        frames = ui.link.screen_record_frames(st, timeout=90)
        ui.shot("window_anim_end")

    print("recorded %d frames at %d fps" % (len(frames), st.get("fps", -1)))
    if len(frames) < 20:
        print("FAIL: too few frames to say anything")
        return 1

    w, h, _ = frames[0]
    base = col_px(frames[0], COLUMN, TOP_SKIP, h)
    edges = [top_edge(col_px(f, COLUMN, TOP_SKIP, h), base) for f in frames]
    # The window only ever RISES, so its life ends at the first increase. That
    # is also how the close ghost is kept out of the open measurement: the ghost
    # is drawn where the window was, so it reads as a top edge that suddenly
    # jumps back down, and without this it counted as another rise position.
    seen, prev = [], None
    for v in edges:
        if v is None:
            if seen:
                break
            continue
        if prev is not None and v > prev:
            break
        seen.append(v)
        prev = v
    print("window top edge on column x=%d:" % COLUMN)
    print("  " + " ".join("-" if v is None else str(v) for v in edges))
    if not seen:
        print("FAIL: the window never appeared on that column")
        return 1
    lo, hi = min(seen), max(seen)
    mids = sorted({v for v in seen if lo < v < hi})
    print("OPEN: %d distinct edge positions (top %d, bottom %d); intermediates: %s"
          % (len(set(seen)), lo, hi, mids if mids else "none"))
    # One intermediate, not two. The recorder lands about three samples across
    # a 130 ms rise - its effective rate is nearer 30 fps than the 60 it is
    # asked for - and the first of them is already partway up, because the
    # launch command's round trip costs some of it. A window that simply
    # APPEARED would give exactly one edge position and zero travel, so one
    # intermediate plus real travel still tells the two apart; demanding two
    # would only be demanding a faster recorder.
    if len(mids) < 1 or hi - lo < 6:
        print("FAIL: the window appeared rather than rising - %d intermediate "
              "position(s) over %d px of travel" % (len(mids), hi - lo))
        return 1

    # CLOSE: anchored to the frame the WINDOW vanished, not to the end of the
    # recording. The ghost lasts ~120 ms and the recording runs for another half
    # second after the close command, so the last N frames are all settled
    # desktop and prove nothing.
    gone = len(seen) + next(i for i, v in enumerate(edges) if v is not None)
    settled = frames[-1][2]
    tail = frames[gone:gone + 16]
    ghost = [band_diff(f[2], settled, w, BAND[0], BAND[1]) for f in tail]
    print("CLOSE: pixels differing from the settled desktop, %d frames from the "
          "one the window vanished (frame %d):" % (len(tail), gone))
    print("  " + " ".join(str(v) for v in ghost))
    if ghost[-1] != 0:
        print("FAIL: the desktop never settled after the close")
        return 1
    moving = [v for v in ghost if v > 0]
    if len(moving) < MIN_INTERMEDIATE:
        print("FAIL: nothing was drawn after the window went - the close ghost "
              "is missing (%d frames showed anything)" % len(moving))
        return 1
    if len(set(moving)) < 2:
        print("FAIL: the ghost never changed size - it is not collapsing")
        return 1
    print("PASS: the window rose through %d positions and the close ghost "
          "collapsed across %d frames" % (len(mids), len(moving)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

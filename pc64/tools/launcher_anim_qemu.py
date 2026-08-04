#!/usr/bin/env python3
"""Prove the Start menu RISES rather than appearing, in QEMU.

Same instrument as tools/snap_anim_qemu.py, and for the same reason: a 110 ms
reveal is shorter than one URC screenshot round trip, so this records on the
guest's own shell tick with `screen record` and reads the ring back afterwards.

THE MEASUREMENT is the menu's TOP EDGE, read off one vertical column: the
highest pixel on that column that differs from the frame before the menu opened.
That is the edge itself, and it should climb through a run of positions.

Counting *how many* pixels on the column differ would have been easier and is
wrong: the first row's hover highlight clears a beat after the menu settles,
which moves that count by a row height long after the animation is over. The
topmost differing pixel is immune - the highlight is inside the menu, below its
top edge - and it is also the quantity actually being animated.

The menu is opened with Ctrl-Esc rather than by clicking the Start chip: the
keyboard path is the one every harness scenario uses, it needs no coordinate
read off a screenshot, and URC carries ctrl (it does not carry alt).

    UNO_DEBUG=1 ./build.sh && python3 tools/launcher_anim_qemu.py [--explore]
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from urcui import UrcUi

ESC_SCAN = 0x17                # pc64_uui.c: `ctrl && scan == 0x17` -> Start menu
COLUMN = 40                    # inside the menu, left of the widest label
MIN_INTERMEDIATE = 2
SETTLE_SLOP = 4                # px of settled chrome drift to ignore (see below)
FPS = 60


def open_menu(ui):
    ui.link.key(ESC_SCAN, 0, 1, timeout=8)


def column(frame, x):
    w, h, rgba = frame
    if x >= w:
        x = w // 2
    return [bytes(rgba[(y * w + x) * 4:(y * w + x) * 4 + 3]) for y in range(h)]


def top_edge(col, base):
    """Highest y on the column differing from `base` (the closed desktop), or
    None when nothing does - i.e. the menu is not visible on this column yet."""
    for y, (p, q) in enumerate(zip(col, base)):
        if p != q:
            return y
    return None


def explore():
    with UrcUi() as ui:
        open_menu(ui)
        time.sleep(1.0)
        ui.shot("launcher_open")
        print("windows:", ui.windows())
    return 0


def main():
    if "--explore" in sys.argv:
        return explore()

    with UrcUi() as ui:
        st0 = ui.link.screen_record_start(1, FPS, timeout=10)
        if st0.get("on") != 1:
            print("FAIL: the recorder did not start")
            return 1
        time.sleep(0.4)                       # settled frames first
        open_menu(ui)
        time.sleep(1.0)
        st = ui.link.screen_record_stop(timeout=10)
        frames = ui.link.screen_record_frames(st, timeout=60)
        ui.shot("launcher_open")

    print("recorded %d frames at %d fps" % (len(frames), st.get("fps", -1)))
    if len(frames) < 8:
        print("FAIL: too few frames to say anything")
        return 1

    base = column(frames[0], COLUMN)
    series = [top_edge(column(f, COLUMN), base) for f in frames]
    seen = [v for v in series if v is not None]
    print("menu top edge on column x=%d, per recorded frame (None = closed):"
          % COLUMN)
    print("  " + " ".join("-" if v is None else str(v) for v in series))

    if not seen:
        print("FAIL: the column never changed - the menu never opened")
        return 1
    lo, hi = min(seen), max(seen)
    mids = sorted({v for v in seen if lo < v < hi})
    print("edge positions: %d distinct (top %d, bottom %d); intermediates: %s"
          % (len(set(seen)), lo, hi, mids if mids else "none"))
    if len(mids) < MIN_INTERMEDIATE:
        print("FAIL: the menu appeared rather than rising - %d intermediate "
              "position(s), wanted %d" % (len(mids), MIN_INTERMEDIATE))
        return 1
    if series[-1] != lo:
        print("FAIL: the last frame is not the settled one (edge %s, want %d)"
              % (series[-1], lo))
        return 1
    # One way only: while it is MOVING, the edge climbs and never drops back.
    # Restricted to frames more than SETTLE_SLOP from the final position,
    # because the settled menu's topmost differing pixel shifts a couple of rows
    # when the first row's hover highlight clears - real, unrelated to the
    # motion, and not something this test should have an opinion about.
    prev = None
    for i, v in enumerate(series):
        if v is None or v - lo <= SETTLE_SLOP:
            continue
        if prev is not None and v > prev:
            print("FAIL: the menu edge moved DOWN at frame %d (%d -> %d)"
                  % (i, prev, v))
            return 1
        prev = v
    print("PASS: the Start menu rose through %d intermediate positions, one way"
          % len(mids))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Scheduler verification: the cooperative per-frame app-tick model.

UnoDOS/IIGS multitasks cooperatively - every app's per-frame tick
(game_tick, pacman_tick, ...) scans the window table for its own window and
advances it, so multiple app windows run concurrently each frame (only the
topmost redraws; all advance their logic). This is the "cooperative-by-ticks"
scheduler the Apple II / SNES ports concluded on.

Proof: open Dostris AND Pac-Man at once; both must advance their state in the
same run even though only one is topmost.

Run from iigs/:  python tests/scheduler.py [path/to/unodos_iigs.po]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Harness  # noqa: E402

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/unodos_iigs.po"
VARS = 0x1000


def w(m, a):
    return m[a] | (m[a + 1] << 8)


def main():
    fails = []
    h = Harness(IMG)
    h.boot()
    h.frames(2)
    # open Dostris (icon cell 4,12 -> px 36,100)
    h.move_to(36, 100)
    h.click()
    h.click()
    h.frames(3)
    # open Pac-Man on top (icon cell 4,16 -> px 36,132)
    h.move_to(36, 132)
    h.click()
    h.click()
    h.frames(3)
    m = h.cpu.mem

    if w(m, VARS + 0x22) != 2:
        fails.append("expected two windows open (Dostris + Pac-Man)")

    dt_y0 = w(m, VARS + 0x44A)      # Dostris piece y
    pm_dots0 = w(m, VARS + 0x470)   # Pac-Man dots remaining
    h.frames(60)
    dt_y1 = w(m, VARS + 0x44A)
    pm_dots1 = w(m, VARS + 0x470)

    if dt_y1 <= dt_y0 and w(m, VARS + 0x44C) == 0:
        # piece either fell or locked+scored; either way Dostris must advance
        fails.append("Dostris (background window) did not advance under the scheduler")
    if pm_dots1 >= pm_dots0:
        fails.append("Pac-Man (topmost window) did not advance under the scheduler")

    if fails:
        print("SCHEDULER FAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print("SCHEDULER PASS  (two app windows advanced concurrently per frame)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

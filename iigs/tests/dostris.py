#!/usr/bin/env python3
"""Dostris (colour Tetris) regression on the headless harness.

Verifies launch, piece movement (left/right/rotate), gravity, hard-drop lock,
and the line-clear + scoring path (by seeding a nearly-full bottom row and
locking a piece so clear_lines fires).

Run from iigs/:  python tests/dostris.py [path/to/unodos_iigs.po]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Harness  # noqa: E402

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/unodos_iigs.po"
VARS = 0x1000
DTBOARD = 0x9A00
V = {  # offsets into VARS
    "state": 0x440, "piece": 0x442, "rot": 0x446,
    "x": 0x448, "y": 0x44A, "score": 0x44C, "lines": 0x44E,
}


def w(m, a):
    return m[a] | (m[a + 1] << 8)


def vv(h, name):
    return w(h.cpu.mem, VARS + V[name])


def launch(h):
    h.boot()
    h.frames(2)
    h.move_to(36, 100)         # Dostris icon (cell 4,12)
    h.click()
    h.click()
    h.frames(3)


def main():
    fails = []
    os.makedirs("shots", exist_ok=True)

    # --- launch ---
    h = Harness(IMG)
    launch(h)
    m = h.cpu.mem
    if m[VARS + 0x70 + 1] != 5:
        fails.append("Dostris (proc 5) window did not open")
    if vv(h, "state") != 0:
        fails.append("game did not start in play state")

    # --- movement: right then left moves x by +1 / -1 ---
    x0 = vv(h, "x")
    h.key(0x15)                # right
    h.frames(1)
    if vv(h, "x") != x0 + 1:
        fails.append(f"right did not move (x {x0}->{vv(h,'x')})")
    h.key(0x08)                # left
    h.frames(1)
    if vv(h, "x") != x0:
        fails.append("left did not move back")
    r0 = vv(h, "rot")
    h.key(0x0B)                # rotate
    h.frames(1)
    if vv(h, "rot") == r0 and vv(h, "piece") != 1:   # O-piece rot is a no-op
        fails.append("rotate did not change rotation")

    # --- hard drop locks 4 cells ---
    occ0 = sum(1 for i in range(180) if m[DTBOARD + i])
    h.key(0x20)                # hard drop
    h.frames(2)
    occ1 = sum(1 for i in range(180) if m[DTBOARD + i])
    if occ1 != occ0 + 4:
        fails.append(f"hard drop did not lock 4 cells ({occ0}->{occ1})")

    # --- gravity: a fresh piece descends over frames ---
    y0 = vv(h, "y")
    h.frames(60)
    if vv(h, "y") <= y0:
        fails.append(f"gravity did not advance (y {y0}->{vv(h,'y')})")
    h.render_png("shots/m3_dostris.png")

    # --- line clear + scoring: seed a full bottom row, then lock a piece ---
    h2 = Harness(IMG)
    launch(h2)
    m2 = h2.cpu.mem
    for c in range(10):                       # fill row 17 completely
        m2[DTBOARD + 17 * 10 + c] = 2
    score0 = vv(h2, "score")
    lines0 = vv(h2, "lines")
    h2.key(0x20)                              # hard drop -> lock -> clear_lines
    h2.frames(3)
    if vv(h2, "score") != score0 + 100:
        fails.append(f"line clear did not score (+100): "
                     f"{score0}->{vv(h2,'score')}")
    if vv(h2, "lines") != lines0 + 1:
        fails.append("line counter did not increment")
    # row 17 must no longer be a complete wall of colour 2 (it cleared/shifted)
    if all(m2[DTBOARD + 17 * 10 + c] == 2 for c in range(10)):
        fails.append("full row was not cleared")

    if fails:
        print("DOSTRIS FAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print("DOSTRIS PASS  (move/rotate/drop/gravity/line-clear)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

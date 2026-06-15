#!/usr/bin/env python3
"""Pac-Man regression: launch, eat dots, ghost motion, and collision.

Run from iigs/:  python tests/pacman.py [path/to/unodos_iigs.po]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Harness  # noqa: E402

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/unodos_iigs.po"
VARS = 0x1000
V = {"state": 0x464, "px": 0x466, "py": 0x468, "dir": 0x46A, "ndir": 0x46C,
     "score": 0x46E, "dots": 0x470, "g0x": 0x476, "g0y": 0x478, "g0d": 0x47A}


def w(m, a):
    return m[a] | (m[a + 1] << 8)


def vv(h, n):
    return w(h.cpu.mem, VARS + V[n])


def launch(h):
    h.boot()
    h.frames(2)
    h.move_to(36, 132)         # Pac-Man icon (cell 4,16)
    h.click()
    h.click()
    h.frames(3)


def main():
    fails = []
    os.makedirs("shots", exist_ok=True)
    h = Harness(IMG)
    launch(h)
    m = h.cpu.mem
    if m[VARS + 0x70 + 1] != 9:
        fails.append("Pac-Man (proc 9) window did not open")
    if vv(h, "dots") < 50:
        fails.append(f"too few dots seeded ({vv(h, 'dots')})")
    if vv(h, "state") != 0:
        fails.append("game did not start in play state")

    g0 = (vv(h, "g0x"), vv(h, "g0y"))
    sc0 = vv(h, "score")
    for k in (0x0A, 0x0A, 0x15, 0x15, 0x0A):   # down,down,right,right,down
        h.key(k)
        h.frames(10)
    if vv(h, "score") <= sc0:
        fails.append("pac did not eat any dots (score unchanged)")
    if (vv(h, "g0x"), vv(h, "g0y")) == g0:
        fails.append("ghost 0 did not move")
    h.render_png("shots/m3_pacman.png")

    # collision: pin pac against a wall at (1,5) and put ghost 0 one tile to
    # the right at (2,5); the ghost greedily steps left onto pac -> dead.
    def poke(n, val):
        m[VARS + V[n]] = val & 0xFF
        m[VARS + V[n] + 1] = 0
    poke("px", 1)
    poke("py", 5)
    poke("dir", 2)             # facing the wall at (0,5): pac can't move
    poke("ndir", 2)
    poke("g0x", 2)
    poke("g0y", 5)
    poke("g0d", 0)            # not facing right, so stepping left isn't reverse
    h.frames(60)
    if vv(h, "state") != 1:
        fails.append(f"ghost collision did not end the game (state={vv(h,'state')})")

    if fails:
        print("PACMAN FAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print("PACMAN PASS  (launch / eat dots / ghost chase / collision)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""OutLast regression: launch, distance advance, steering, road render.

Run from iigs/:  python tests/outlast.py [path/to/unodos_iigs.po]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Harness, SHR_PIX, ROWBYTES  # noqa: E402

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/unodos_iigs.po"
VARS = 0x1000
DIST = VARS + 0x488
CARX = VARS + 0x486


def w(m, a):
    return m[a] | (m[a + 1] << 8)


def px(m, x, y):
    b = m[SHR_PIX + y * ROWBYTES + (x >> 1)]
    return (b >> 4) if (x & 1) == 0 else (b & 0x0F)


def main():
    fails = []
    os.makedirs("shots", exist_ok=True)
    h = Harness(IMG)
    h.boot()
    h.frames(2)
    h.move_to(132, 132)        # OutLast icon (cell 16,16)
    h.click()
    h.click()
    h.frames(3)
    m = h.cpu.mem
    if m[VARS + 0x70 + 1] != 10:
        fails.append("OutLast (proc 10) window did not open")

    d0 = w(m, DIST)
    h.frames(30)
    if w(m, DIST) <= d0:
        fails.append("distance did not advance")

    c0 = w(m, CARX) & 0xFFFF
    for _ in range(5):
        h.key(0x15)            # steer right
        h.frames(1)
    if (w(m, CARX) & 0xFFFF) == c0:
        fails.append("steering right did not move the car")
    for _ in range(8):
        h.key(0x08)            # steer left past centre
        h.frames(1)
    h.render_png("shots/m3_outlast.png")

    # the road raster: sky blue near the top, green grass at a corner, grey
    # road down the centre.  Window proc-10 def: x=2,y=2 -> canvas origin px.
    # canvas spans px x 32..304, y 24..184 (320x200 framebuffer)
    if px(m, 120, 30) != 13:           # sky band
        fails.append(f"sky band not blue (idx {px(m,120,30)})")
    if px(m, 40, 170) != 10:           # far-left grass near the bottom
        fails.append(f"grass not green at bottom-left (idx {px(m,40,170)})")
    mid = px(m, 160, 170)
    if mid != 12 and mid != 1:         # centre road: grey or white stripe
        fails.append(f"road not grey/striped at centre-bottom (idx {mid})")

    if fails:
        print("OUTLAST FAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print("OUTLAST PASS  (launch / distance / steering / road raster)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

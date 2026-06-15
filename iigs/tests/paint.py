#!/usr/bin/env python3
"""Paint regression: ink select, drag-paint, and clear.

Run from iigs/:  python tests/paint.py [path/to/unodos_iigs.po]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Harness  # noqa: E402

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/unodos_iigs.po"
VARS = 0x1000
PAINTBUF = 0x9C00
INK = VARS + 0x454


def w(m, a):
    return m[a] | (m[a + 1] << 8)


def main():
    fails = []
    os.makedirs("shots", exist_ok=True)
    h = Harness(IMG)
    h.boot()
    h.frames(2)
    h.move_to(132, 100)        # Paint icon (cell 16,12)
    h.click()
    h.click()
    h.frames(3)
    m = h.cpu.mem
    if m[VARS + 0x70 + 1] != 6:
        fails.append("Paint (proc 6) window did not open")

    # default canvas is all-grey (index 15)
    if any(m[PAINTBUF + i] != 15 for i in range(36 * 18)):
        fails.append("canvas did not start blank (all index 15)")

    # pick ink '7' -> paint_inks[6] = 11 (red)
    h.key(ord("7"))
    h.frames(1)
    if w(m, INK) != 11:
        fails.append(f"ink select failed (ink={w(m, INK)}, want 11)")

    # drag-paint: hold the button and move across a few canvas cells
    h.mouse_btn = 1
    for x in (40, 64, 96, 128):
        h.move_to(x, 60)
        h.frames(1)
    h.mouse_btn = 0
    h.frames(1)
    red = sum(1 for i in range(36 * 18) if m[PAINTBUF + i] == 11)
    if red < 3:
        fails.append(f"drag-paint laid down too few cells ({red})")
    h.render_png("shots/m3_paint.png")

    # clear resets the canvas
    h.key(ord("C"))
    h.frames(2)
    if any(m[PAINTBUF + i] != 15 for i in range(36 * 18)):
        fails.append("C did not clear the canvas")

    if fails:
        print("PAINT FAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print("PAINT PASS  (ink select / drag-paint / clear)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

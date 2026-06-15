#!/usr/bin/env python3
"""M1 regression: desktop, window manager, keyboard + mouse, SysInfo + Clock.

Drives the headless harness and asserts both framebuffer content (palette
indices at known points) and kernel state (bank-0 VARS): a window launches
by keyboard AND by mouse double-click, the clock advances, a title-bar drag
moves a window, and the close box destroys it.

Run from iigs/:  python tests/m1.py [path/to/unodos_iigs.po]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Harness, SHR_PIX, ROWBYTES  # noqa: E402

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/unodos_iigs.po"
VARS = 0x1000
WT = VARS + 0x70           # window table (bank 0)


def px(m, x, y):
    b = m[SHR_PIX + y * ROWBYTES + (x >> 1)]
    return (b >> 4) if (x & 1) == 0 else (b & 0x0F)


def word(m, a):
    return m[a] | (m[a + 1] << 8)


def main():
    fails = []
    h = Harness(IMG)
    h.boot()
    h.frames(2)
    m = h.cpu.mem

    # --- desktop: white menu bar (index 1) at top-right, blue desktop body ---
    if px(m, 300, 4) != 1:
        fails.append(f"menu bar not white at (300,4): idx {px(m,300,4)}")
    if px(m, 300, 120) != 0:
        fails.append(f"desktop not blue at (300,120): idx {px(m,300,120)}")
    os.makedirs("shots", exist_ok=True)
    h.render_png("shots/m1_desktop.png")

    # --- keyboard: select first icon (right) then launch (return) ---
    h.key(0x15)
    h.frames(1)
    if word(m, VARS + 0x24) != 0:
        fails.append(f"right-arrow did not select icon 0 (sel={word(m,VARS+0x24)})")
    h.key(0x0D)
    h.frames(3)
    if word(m, VARS + 0x22) != 1:
        fails.append("return did not launch a window (zcount != 1)")
    if m[WT + 0] != 1 or m[WT + 1] != 0:
        fails.append("slot 0 is not an active SysInfo (proc 0) window")
    h.render_png("shots/m1_sysinfo.png")

    # close it (ESC) to reset
    h.key(0x1B)
    h.frames(2)
    if word(m, VARS + 0x22) != 0:
        fails.append("ESC did not close the topmost window")

    # --- mouse: double-click the Clock icon (cell 16,4 -> px ~132,36) ---
    h.move_to(132, 36)
    h.click()
    h.click()
    h.frames(2)
    if word(m, VARS + 0x22) != 1:
        fails.append("mouse double-click did not launch the Clock")
    if m[WT + 1] != 1:
        fails.append("launched window is not Clock (proc 1)")
    # advance ~3 s and confirm the clock ticked
    h.frames(190)
    if word(m, VARS + 0x06) < 2:
        fails.append(f"clock did not advance (secs={word(m,VARS+0x06)})")
    h.render_png("shots/m1_clock.png")

    # --- drag: grab the title bar and move the window ---
    wx0 = word(m, WT + 2)
    h.move_to((word(m, WT + 2) + 2) * 8, word(m, WT + 4) * 8 + 4)
    h.mouse_btn = 1
    h.frames(1)
    h.move_to(60, 140)
    h.frames(2)
    h.mouse_btn = 0
    h.frames(2)
    if word(m, WT + 2) == wx0 and word(m, WT + 4) == 8:
        fails.append("title-bar drag did not move the window")

    # --- close box: click the 'X' at the window's new top-right ---
    wx = word(m, WT + 2)
    wy = word(m, WT + 4)
    ww = word(m, WT + 6)
    h.move_to((wx + ww - 2) * 8 + 4, wy * 8 + 4)
    h.click()
    h.frames(3)
    if word(m, VARS + 0x22) != 0:
        fails.append("close box did not destroy the window")

    if fails:
        print("M1 FAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print(f"M1 PASS  ({h.cpu.cycles} instrs, {h.frame} frames)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""M0 regression: boot the image headlessly and assert the SHR splash.

Checks the boot chain reached the kernel (CPU halted on STP), SHR was
enabled, and the framebuffer holds the expected UnoDOS layout by sampling
palette-index values at known points - desktop, menu bar, panel frame,
panel body, and that the panel title bar actually contains rendered text.

Run from iigs/:  python tests/m0.py [path/to/unodos_iigs.po]
Exit code 0 = pass.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Harness, SHR_PIX, ROWBYTES  # noqa: E402

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/unodos_iigs.po"


def px(mem, x, y):
    """palette index of SHR pixel (x,y), 320x200, high nibble = left."""
    b = mem[SHR_PIX + y * ROWBYTES + (x >> 1)]
    return (b >> 4) if (x & 1) == 0 else (b & 0x0F)


def main():
    h = Harness(IMG)
    h.run()
    m = h.cpu.mem
    fails = []

    if not h.cpu.halted:
        fails.append("CPU never halted (boot chain did not reach the kernel STP)")
    if not (h.newvideo & 0x80):
        fails.append(f"SHR not enabled (NEWVIDEO=${h.newvideo:02X})")

    # sampled palette indices (see kernel.s splash layout)
    samples = [
        ("desktop blue", 8, 185, 0),
        ("menu bar grey", 300, 4, 5),          # far right, clear of the title
        ("panel frame black", 61, 100, 4),     # left frame column (px ~60)
        ("panel title deep-blue", 240, 47, 6), # title bar, right of the text
        ("panel body cyan", 200, 120, 2),
    ]
    for name, x, y, want in samples:
        got = px(m, x, y)
        if got != want:
            fails.append(f"{name} at ({x},{y}): got index {got}, want {want}")

    # the panel title bar must contain white (index 1) text pixels
    title_white = any(px(m, x, 47) == 1 for x in range(68, 200))
    if not title_white:
        fails.append("no white text pixels found in the panel title bar")

    if fails:
        print("M0 FAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print(f"M0 PASS  ({h.cpu.cycles} instrs, NEWVIDEO=${h.newvideo:02X}, halted)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

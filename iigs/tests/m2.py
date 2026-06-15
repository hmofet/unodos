#!/usr/bin/env python3
"""M2 regression: FAT12 over SmartPort + Files + Notepad with persistence.

Drives the headless harness: open Files (lists the FAT12 root directory),
open WELCOME.TXT into Notepad (single-cluster read), open CHAIN.TXT
(multi-cluster read), then edit + Ctrl-S a file, write the disk image back,
reboot from it, and confirm the edit survived.

Run from iigs/:  python tests/m2.py [path/to/unodos_iigs.po]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Harness  # noqa: E402

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/unodos_iigs.po"
VARS = 0x1000
NBUF = 0xA000


def word(m, a):
    return m[a] | (m[a + 1] << 8)


def open_files(h):
    h.boot()
    h.frames(2)
    h.move_to(36, 68)          # the Files desktop icon (cell 4,8)
    h.click()
    h.click()
    h.frames(3)


def main():
    fails = []
    os.makedirs("shots", exist_ok=True)

    # --- Files lists the FAT12 root directory ---
    h = Harness(IMG)
    open_files(h)
    m = h.cpu.mem
    if h.cpu.mem[VARS + 0x70 + 1] != 7:
        fails.append("Files (proc 7) window did not open")
    if word(m, VARS + 0x300) != 3:
        fails.append(f"expected 3 directory entries, got {word(m, VARS+0x300)}")
    h.render_png("shots/m2_files.png")

    # --- open WELCOME.TXT (single cluster) ---
    h.key(0x0D)
    h.frames(4)
    ln = word(m, VARS + 0x306)
    if ln != 223:
        fails.append(f"WELCOME.TXT length {ln}, expected 223")
    if bytes(m[NBUF:NBUF + 7]) != b"Welcome":
        fails.append("WELCOME.TXT content not read into NBUF")
    h.render_png("shots/m2_notepad.png")

    # --- open CHAIN.TXT (multi-cluster, crosses a 1 KB cluster boundary) ---
    h2 = Harness(IMG)
    open_files(h2)
    m2 = h2.cpu.mem
    h2.key(0x0A)
    h2.key(0x0A)               # select the 3rd entry (CHAIN.TXT)
    h2.frames(1)
    h2.key(0x0D)
    h2.frames(5)
    cl = word(m2, VARS + 0x306)
    if cl != 2160:
        fails.append(f"CHAIN.TXT length {cl}, expected 2160")
    if bytes(m2[NBUF + 1100:NBUF + 1115]) != b"est on the Mac.":
        fails.append("CHAIN.TXT multi-cluster data mismatch past 1 KB")

    # --- edit + save + reboot persistence ---
    h3 = Harness(IMG)
    open_files(h3)
    h3.key(0x0D)
    h3.frames(4)               # open WELCOME.TXT
    for c in " [EDIT]":
        h3.key(ord(c))
    h3.key(0x13)               # Ctrl-S
    h3.frames(6)
    if word(h3.cpu.mem, VARS + 0x308) != 0:
        fails.append("Notepad still dirty after Ctrl-S (save failed)")
    wb = "build/m2_writeback.po"
    open(wb, "wb").write(h3.image)

    h4 = Harness(wb)
    open_files(h4)
    h4.key(0x0D)
    h4.frames(4)
    m4 = h4.cpu.mem
    ln4 = word(m4, VARS + 0x306)
    if ln4 != 230:
        fails.append(f"after reboot WELCOME.TXT length {ln4}, expected 230")
    if bytes(m4[NBUF + ln4 - 7:NBUF + ln4]) != b" [EDIT]":
        fails.append(f"edit did not persist across reboot "
                     f"(tail={bytes(m4[NBUF+ln4-7:NBUF+ln4])})")
    h4.render_png("shots/m2_persist.png")

    if fails:
        print("M2 FAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print("M2 PASS  (FAT12 list/read/multi-cluster/save+persist)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

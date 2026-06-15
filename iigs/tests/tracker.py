#!/usr/bin/env python3
"""Tracker regression: pattern edit, cursor nav, and 4-voice DOC playback.

Run from iigs/:  python tests/tracker.py [path/to/unodos_iigs.po]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Harness  # noqa: E402

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/unodos_iigs.po"
VARS = 0x1000
TRKPAT = 0x9F00
STEP = VARS + 0x458
CHAN = VARS + 0x45A
PLAY = VARS + 0x45C
POS = VARS + 0x45E


def w(m, a):
    return m[a] | (m[a + 1] << 8)


def main():
    fails = []
    os.makedirs("shots", exist_ok=True)
    h = Harness(IMG)
    h.boot()
    h.frames(2)
    h.move_to(212, 100)        # Tracker icon (cell 26,12)
    h.click()
    h.click()
    h.frames(3)
    m = h.cpu.mem
    if m[VARS + 0x70 + 1] != 8:
        fails.append("Tracker (proc 8) window did not open")

    # edit: set note 4 at the cursor (step 0, chan 0)
    h.key(ord("4"))
    h.frames(1)
    if m[TRKPAT + 0] != 4:
        fails.append(f"note edit failed (TRKPAT[0]={m[TRKPAT+0]})")

    # cursor: right moves step, down moves channel
    h.key(0x15)
    h.key(0x0A)
    h.frames(1)
    if w(m, STEP) != 1 or w(m, CHAN) != 1:
        fails.append(f"cursor nav failed (step={w(m,STEP)} chan={w(m,CHAN)})")
    h.key(ord("7"))
    h.frames(1)
    if m[TRKPAT + 1 * 4 + 1] != 7:    # step 1, chan 1
        fails.append("note not written at moved cursor")

    # space clears the cell
    h.key(0x20)
    h.frames(1)
    if m[TRKPAT + 1 * 4 + 1] != 0:
        fails.append("space did not clear the note")

    # playback: P toggles, tracker_tick advances + drives the DOC
    n0 = len(h.doc_writes)
    h.key(ord("p"))
    h.frames(40)
    if w(m, PLAY) != 1:
        fails.append("P did not start playback")
    if w(m, POS) == 0:
        fails.append("play position did not advance")
    osc_writes = [(r, v) for (r, v) in h.doc_writes[n0:]
                  if r in (0, 1, 2, 3, 0x20, 0x21, 0x22, 0x23, 0xA0, 0xA1, 0xA2, 0xA3)]
    if len(osc_writes) < 4:
        fails.append(f"playback did not drive the DOC voices ({len(osc_writes)})")
    h.render_png("shots/m3_tracker.png")

    if fails:
        print("TRACKER FAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print("TRACKER PASS  (edit / cursor / 4-voice DOC playback)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

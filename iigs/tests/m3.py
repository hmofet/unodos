#!/usr/bin/env python3
"""M3 regression: 4096-colour Theme + Ensoniq DOC audio (Music).

Theme is verified visually (the SHR palette changes recolour the whole
desktop) and by reading the palette back.  DOC audio isn't audible in the
harness (no synthesis), but the harness logs every sound-GLU register write,
so we assert the Music app programs oscillator 0 with the melody's frequency
words.

Run from iigs/:  python tests/m3.py [path/to/unodos_iigs.po]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Harness  # noqa: E402

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/unodos_iigs.po"
VARS = 0x1000
SHR_PAL = 0xE1_9E00


def word(m, a):
    return m[a] | (m[a + 1] << 8)


def main():
    fails = []
    os.makedirs("shots", exist_ok=True)

    # --- Theme: open it, cycle to preset 3 (Sunset), check the palette ---
    h = Harness(IMG)
    h.boot()
    h.frames(2)
    h.move_to(132, 68)         # Theme icon (cell 16,8)
    h.click()
    h.click()
    h.frames(3)
    if h.cpu.mem[VARS + 0x70 + 1] != 3:
        fails.append("Theme (proc 3) window did not open")
    h.key(0x15)
    h.key(0x15)
    h.key(0x15)                # right x3 -> preset 3 (Sunset)
    h.frames(2)
    if word(h.cpu.mem, VARS + 0x330) != 3:
        fails.append("Theme preset index did not advance to 3")
    if word(h.cpu.mem, SHR_PAL) != 0x0700:
        fails.append(f"Sunset desktop colour wrong: "
                     f"${word(h.cpu.mem, SHR_PAL):04X} (want $0700)")
    h.render_png("shots/m3_theme.png")

    # --- Music: open it, let the melody run, check DOC oscillator-0 writes ---
    h2 = Harness(IMG)
    h2.boot()
    h2.frames(2)
    n_init = len(h2.doc_writes)
    if n_init < 33:
        fails.append(f"doc_init issued too few writes ({n_init}); "
                     "expected >=33 (32 halts + osc-enable)")
    h2.move_to(212, 68)        # Music icon (cell 26,8)
    h2.click()
    h2.click()
    h2.frames(40)              # advance a couple of notes
    if h2.cpu.mem[VARS + 0x70 + 1] != 4:
        fails.append("Music (proc 4) window did not open")
    play = h2.doc_writes[n_init:]
    freq_lo = [v for (r, v) in play if r == 0x00]
    ctrl = [v for (r, v) in play if r == 0xA0]
    # melody note $0240 -> freq-low byte $40; note $0280 -> $80; etc.
    if 0x40 not in freq_lo or 0x80 not in freq_lo:
        fails.append(f"Music did not program osc-0 frequencies "
                     f"(freq-lo writes: {freq_lo[:6]})")
    if 0x00 not in ctrl:
        fails.append("Music never started osc-0 (no free-run control write)")
    h2.render_png("shots/m3_music.png")

    if fails:
        print("M3 FAIL:")
        for f in fails:
            print("  -", f)
        return 1
    print("M3 PASS  (Theme palette recolour + Ensoniq DOC oscillator path)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

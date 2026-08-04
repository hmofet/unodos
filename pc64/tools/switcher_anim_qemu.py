#!/usr/bin/env python3
"""Prove the Alt-Tab switcher's selection highlight SLIDES between cells.

Same instrument as the snap and launcher gates - the guest-side `screen record`
ring - because a 90 ms slide is shorter than one URC screenshot round trip.

Driven with Ctrl-Tab, not Alt-Tab: URC's key verb carries ctrl but not alt, and
Ctrl-Tab opens and steps the SAME overlay in the same MRU order (it is the
documented fallback for transports that cannot report Alt). It commits on a
~0.8 s timer after the last step, so the recording window is shorter than that.

THE MEASUREMENT self-calibrates rather than hard-coding the overlay's geometry.
It looks for a cell-wide single-colour run in the middle band of the screen,
and accepts a candidate only once its position is seen to CHANGE across the
recording - which is what tells the sliding highlight apart from every static
panel that happens to be a cell wide. Each frame then reports where that run
starts: the highlight's left edge, precisely what is animated.

    UNO_DEBUG=1 ./build.sh && python3 tools/switcher_anim_qemu.py
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from urcui import UrcUi

TAB_UNI = 0x09                 # pc64_uui.c: `ctrl && uni == 0x09` -> the switcher
CELL_W = 96                    # SW_CELL_W; the run we are looking for
CELL_SLOP = 8                  # a run within this of CELL_W may be the cell
MIN_INTERMEDIATE = 2
FPS = 60


def step(ui):
    ui.link.key(0, TAB_UNI, 1, timeout=8)


def row(frame, y):
    w, h, rgba = frame
    return [bytes(rgba[(y * w + x) * 4:(y * w + x) * 4 + 3]) for x in range(w)]


def runs(px):
    """Every single-colour run in a row, as (length, start, colour)."""
    out, i = [], 0
    while i < len(px):
        j = i
        while j < len(px) and px[j] == px[i]:
            j += 1
        out.append((j - i, i, px[i]))
        i = j
    return out


def candidates(frame):
    """(row, colour) pairs carrying a cell-wide run, best width match first.

    Deduplicated BY COLOUR, not by row: the same run appears on dozens of
    adjacent rows, and a list of (row, colour) pairs fills up with one element
    seen many times before it ever reaches the highlight. Width alone cannot
    identify the highlight anyway - the caller then keeps only a candidate that
    actually MOVES."""
    w, h, _ = frame
    best = {}
    for y in range(h // 5, (4 * h) // 5):
        for ln, st, col in runs(row(frame, y)):
            d = abs(ln - CELL_W)
            if d <= CELL_SLOP and (col not in best or d < best[col][0]):
                best[col] = (d, y)
    return [(y, col) for col, (d, y) in sorted(best.items(), key=lambda kv: kv[1][0])]


def cell_left(px, colour):
    """Start of the cell-wide run of `colour` on this row, or None.

    Deliberately NOT "the leftmost pixel of that colour": the accent is a
    palette role, so the same blue fills the selected row of any list on screen.
    The Files window's selected row is 35 px of exactly this colour and sits to
    the LEFT of the switcher, so a leftmost-pixel scan locked onto something
    that never moves and the gate concluded nothing was animating."""
    for ln, st, col in runs(px):
        if col == colour and abs(ln - CELL_W) <= CELL_SLOP:
            return st
    return None


def main():
    with UrcUi() as ui:
        # THREE windows, deliberately. With one open app the switcher refuses to
        # appear at all ("nothing to switch TO"), and with two the second step
        # wraps back leftwards; three makes the step under test an unambiguous
        # move to the right, which is what the one-way assertion below reads.
        for slot in (2, 3):                   # Files, System
            ui.link.command("launch", slot, timeout=15)
            time.sleep(2.0)
        if len(ui.windows()) < 3:
            print("FAIL: wanted three windows, got %r" % (ui.windows(),))
            return 1

        # BOTH presses go inside one recording, and close together. The overlay
        # commits ~0.8 s after the last step, and starting the recorder between
        # the two presses spent enough of that budget on URC round trips that
        # the overlay had already committed - so the second press re-OPENED it,
        # which sets the highlight with no slide, and the gate saw nothing move.
        st0 = ui.link.screen_record_start(1, FPS, timeout=10)
        if st0.get("on") != 1:
            print("FAIL: the recorder did not start")
            return 1
        time.sleep(0.20)
        step(ui)                              # opens the overlay on cell 1
        time.sleep(0.25)
        step(ui)                              # steps to cell 2: the slide
        time.sleep(0.45)                      # stop before the commit closes it
        st = ui.link.screen_record_stop(timeout=10)
        frames = ui.link.screen_record_frames(st, timeout=60)
        ui.shot("switcher_open")

    print("recorded %d frames at %d fps" % (len(frames), st.get("fps", -1)))
    if len(frames) < 8:
        print("FAIL: too few frames to say anything")
        return 1

    # Calibrate on frames from across the recording, not just the last one: the
    # overlay commits on a timer, so the tail may have none. A candidate is
    # accepted only if its left edge actually takes several positions, which is
    # what distinguishes the sliding highlight from every static panel that
    # happens to be a cell wide.
    y = colour = None
    series = None
    tried = 0
    for probe in (len(frames) - 1, (3 * len(frames)) // 4, len(frames) // 2):
        for cy, ccol in candidates(frames[probe])[:16]:
            tried += 1
            s = [cell_left(row(f, cy), ccol) for f in frames]
            if len({v for v in s if v is not None}) >= 3:
                y, colour, series = cy, ccol, s
                break
        if series:
            break
    if not series:
        print("FAIL: no cell-width colour run moves across the recording "
              "(%d candidates tried) - the overlay was never up, or the "
              "highlight is not animating at all" % tried)
        return 1
    print("calibrated: row y=%d, highlight colour %s (%d candidates tried)"
          % (y, colour.hex(), tried))
    print("highlight left edge, per recorded frame (None = not on this row):")
    print("  " + " ".join("-" if v is None else str(v) for v in series))

    seen = [v for v in series if v is not None]
    if not seen:
        print("FAIL: the highlight was never found")
        return 1
    lo, hi = min(seen), max(seen)
    mids = sorted({v for v in seen if lo < v < hi})
    print("edge positions: %d distinct (left %d, right %d); intermediates: %s"
          % (len(set(seen)), lo, hi, mids if mids else "none"))
    if hi - lo < CELL_W // 2:
        print("FAIL: the highlight barely moved (%d px) - it should have "
              "crossed a whole cell" % (hi - lo))
        return 1
    if len(mids) < MIN_INTERMEDIATE:
        print("FAIL: the highlight jumped rather than slid - %d intermediate "
              "position(s), wanted %d" % (len(mids), MIN_INTERMEDIATE))
        return 1
    # One way: it steps forward, so the edge only ever moves right.
    prev = None
    for i, v in enumerate(series):
        if v is None:
            continue
        if prev is not None and v < prev:
            print("FAIL: the highlight moved BACKWARDS at frame %d (%d -> %d) - "
                  "two tweens are fighting over it" % (i, prev, v))
            return 1
        prev = v
    print("PASS: the highlight slid %d px through %d intermediate positions, "
          "one way" % (hi - lo, len(mids)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

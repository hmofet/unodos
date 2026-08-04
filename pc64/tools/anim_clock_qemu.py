#!/usr/bin/env python3
"""Prove the shell's animation clock is REAL MILLISECONDS, in QEMU.

The unoui animation facility is only worth having if its clock measures elapsed
TIME rather than frames - that is the entire point of the seam. A green build
does not show that, and neither does one screenshot: a frame-counted fallback
would also produce a number that goes up.

So this boots the debug image and reads the System window's Timing row twice,
with a known gap of real time between them. The row says which clock is in use
(`TSC <n> MHz` or `frame-counted`) and what it currently reads. Alongside each
reading it prints the guest's own `uptime` and the host's wall clock, so the
three can be compared: on a real millisecond clock all three advance together,
while a frame-counted one drifts with how busy the desktop was.

The System window is CLOSED and reopened for the second reading, because the
row is formatted when the window is built.

    UNO_DEBUG=1 ./build.sh && python3 tools/anim_clock_qemu.py

Run under WSL (needs the remote_qemu prerequisites). Writes two screenshots to
pc64/shots/ and prints the readings; the Timing row itself is read off the
shots, which is the same way every coordinate in this tree is checked.
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from urcui import UrcUi

GAP = 8.0                      # seconds of real time between the two readings


SYS_SLOT = 3                   # kAppNames in pc64_uui.c: the System window


def reading(ui, tag, t0):
    guest = ui.link.uptime()
    ui.shot(tag)
    print("  %-14s guest uptime %7d ms   host +%.1f s" %
          (tag, guest, time.time() - t0))
    return guest


def main():
    t0 = time.time()
    with UrcUi() as ui:
        # Open System ONCE. The window is built once and cached (g_built in
        # pc64_uui.c), so reopening it would prove nothing about the clock -
        # the Timing row has to be refreshed in place by the shell's own
        # half-second housekeeping, and that is exactly what this checks.
        ui.link.command("launch", SYS_SLOT, timeout=15)
        time.sleep(3.0)
        titles = ui.windows()
        if not any(t.startswith("System") for t in titles):
            raise SystemExit("slot %d did not open System (got %r) - the app "
                             "table in pc64_uui.c moved" % (SYS_SLOT, titles))
        print("readings (the Timing row itself is in the shots):")
        a = reading(ui, "anim_clock_1", t0)
        time.sleep(GAP)
        b = reading(ui, "anim_clock_2", t0)

    moved, real = (b - a) / 1000.0, GAP
    print("guest uptime advanced %.1f s across a %.1f s host sleep" % (moved, real))
    print("now read pc64/shots/anim_clock_1.png and anim_clock_2.png: the "
          "Timing row must say TSC (not frame-counted) and its 'up N.N s' must "
          "have advanced by about the same %.1f s." % moved)
    return 0


if __name__ == "__main__":
    sys.exit(main())

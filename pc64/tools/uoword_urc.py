#!/usr/bin/env python3
"""Prove UnoWord's MOUSE works: menus, toolbar buttons and dialogs, clicked.

The QMP/usb-tablet harness (uoword_verify.py) can drive UnoWord's keyboard
but not its pointer - QEMU sends this guest no pointer motion - so the menus
and every dialog stayed unverified.  This drives them over URC instead, with
real injected clicks at real framebuffer coordinates.

    python3 tools/uoword_urc.py     # needs a UNO_DEBUG=1 build in build/esp
"""
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from urcui import UrcUi

fails = []


def check(cond, label, detail=""):
    print(("  ok   " if cond else "  FAIL ") + label + (("  " + detail) if detail and not cond else ""))
    if not cond:
        fails.append(label)


def main():
    with UrcUi() as ui:
        w, h = ui.size()
        print("desktop is %dx%d" % (w, h))

        slot = ui.launch_named("UnoWord", settle=6.0)
        print("UnoWord is app slot %d of %d" % (slot, ui.app_count()))
        ui.shot("urc_00_open")

        # The window is opened by the shell at a known spot: uw_build asks for
        # workarea-40 x workarea-60 at (20,16).  The canvas starts just inside
        # the frame, so the menu bar sits a little below the title bar.
        # Rather than hardcode the chrome's internals, click a generous point
        # inside each menu title and let the screenshots show the truth.
        mb_y = 67   # the menu bar band, read off urc_00_open.png (y=40 was the title bar)
        ui.click(54, mb_y)             # File
        ui.shot("urc_01_file_menu")

        ui.key(27)                     # Esc
        time.sleep(0.3)

        ui.click(184, mb_y)            # Format
        ui.shot("urc_02_format_menu")

        # Format > Font... is the first item
        ui.click(190, 87)
        ui.shot("urc_03_font_dialog")

        # INSIDE the dialog: tick Bold, then press OK.  Coordinates read off
        # urc_03_font_dialog.png - the dialog centres itself, so they are
        # stable for a given screen size.
        ui.click(247, 137)             # the Bold check box
        ui.shot("urc_04_dialog_checked")
        ui.click(288, 240)             # the OK button
        time.sleep(0.5)
        ui.shot("urc_05_dialog_ok")

        # a toolbar button: Bold on the Formatting bar
        ui.click(194, 111)
        ui.shot("urc_06_bold_clicked")

        # Help > About: a message box, driven entirely by mouse
        ui.click(285, mb_y)
        ui.shot("urc_07_help_menu")
        ui.click(292, 87)
        ui.shot("urc_08_about")
        ui.key(13)                     # Enter dismisses it
        time.sleep(0.4)
        ui.shot("urc_09_final")

    print("\n%s" % ("urc ui: %d FAILURE(S)" % len(fails) if fails else "urc ui: script completed"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

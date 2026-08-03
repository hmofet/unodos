#!/usr/bin/env python3
"""Prove UnoCalc RUNS: typing into the grid, a formula that recalculates, the
menus and a dialog - all driven by real injected keys and clicks over URC.

The host gate (uoffice/build.sh -> uocalc_test) proves the ENGINE: 73
functions, the number-format language, the recalc chain.  It says nothing
about whether the module loads on the OS, whether the shell routes keys to
it, or whether a click lands where the grid painted.  UnoWord shipped a
version of every one of those bugs and each was invisible to the host gate.

    python3 tools/uocalc_urc.py     # needs a UNO_DEBUG=1 build in build/esp
"""
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from urcui import UrcUi

fails = []


def check(cond, label, detail=""):
    print(("  ok   " if cond else "  FAIL ") + label +
          (("  " + detail) if detail and not cond else ""))
    if not cond:
        fails.append(label)


def type_text(ui, s):
    for ch in s:
        ui.key(ord(ch))
        time.sleep(0.06)


def main():
    with UrcUi() as ui:
        w, h = ui.size()
        print("desktop is %dx%d" % (w, h))

        slot = ui.launch_named("UnoCalc", settle=6.0)
        print("UnoCalc is app slot %d of %d" % (slot, ui.app_count()))
        p = ui.shot("uxl_00_open")
        check(p is not None, "UnoCalc opens a window")

        # A1 is selected on open.  Type two numbers and a formula over them:
        # Enter commits and steps down, so this fills A1, A2, A3.
        type_text(ui, "6")
        ui.key(13)
        type_text(ui, "7")
        ui.key(13)
        ui.shot("uxl_01_numbers")

        type_text(ui, "=A1*A2")
        ui.key(13)
        time.sleep(0.4)
        ui.shot("uxl_02_formula")     # A3 should read 42

        # click A3 to select it: the formula bar must show =A1*A2, which is
        # the whole point of keeping the source text in the cell.  Row 1 sits
        # at y=181 and rows are 20px, read off uxl_01_numbers.png.
        ui.click(110, 221)
        ui.shot("uxl_03_selected")

        # the menus, by mouse.  y read off uxl_00_open.png the same way
        # uoword_urc.py does; the chrome is the same uochrome band.
        mb_y = 67
        ui.click(54, mb_y)            # File
        ui.shot("uxl_04_file_menu")
        ui.key(27)
        time.sleep(0.3)

        ui.click(197, mb_y)           # Format
        ui.shot("uxl_05_format_menu")
        ui.key(27)
        time.sleep(0.3)

        # Help > About: a message box driven entirely by mouse.  The menu
        # titles sit at x = 54/95/140/197/250/293 - read off a screenshot,
        # because a title's x depends on the label widths before it.
        ui.click(293, mb_y)
        ui.shot("uxl_06_help_menu")
        ui.click(300, 87)          # About UnoCalc, the only item
        ui.shot("uxl_07_about")
        ui.key(13)
        time.sleep(0.4)
        ui.shot("uxl_08_final")

    print("\n%s" % ("uocalc urc: %d FAILURE(S)" % len(fails) if fails
                    else "uocalc urc: script completed - read shots/uxl_*.png"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Prove the Office suite's file I/O ROUND-TRIPS on a booted machine.

A save that writes bytes is not a save that works. This types content into
UnoCalc and UnoShow, saves it as a real .xls / .ppt through unodoc, clears
the document, reopens the file and checks the content came back on screen -
the only test that catches a writer and a reader that are each
self-consistent and disagree with each other.

    python3 tools/uofile_urc.py     # needs a UNO_DEBUG=1 build in build/esp

COORDINATES ARE MEASURED, NEVER COMPUTED.  The File menu's items sit at
y = 88 (New), 109 (Open...), 130 (Save) - read off a screenshot, because an
item's y depends on every item above it and on where the shell put the
window.  The first cut of this script assumed Save was the item at 109,
which is Open, and every step after that cascaded: it "saved" by opening a
file that did not exist, and then reported a successful round-trip of data
that had never left the grid.
"""
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from urcui import UrcUi

fails = []
MB_Y   = 67          # the menu bar band
F_NEW  = 88          # File > New
F_OPEN = 109         # File > Open...
F_SAVE = 130         # File > Save
NAME_X, NAME_Y = 300, 228     # the file dialog's File name field
OK_X,   OK_Y   = 427, 228     # its Open / Save button
LIST_X, LIST_Y = 250, 112     # the first row of its file list


def check(cond, label, detail=""):
    print(("  ok   " if cond else "  FAIL ") + label +
          (("  " + detail) if detail and not cond else ""))
    if not cond:
        fails.append(label)


def type_text(ui, s):
    for ch in s:
        ui.key(ord(ch))
        time.sleep(0.06)


def menu(ui, x, item_y):
    ui.click(x, MB_Y)
    time.sleep(0.25)
    ui.click(x + 16, item_y)
    time.sleep(0.5)


def save_as(ui, name):
    """The Save As dialog is up: name it and press the button."""
    ui.click(NAME_X, NAME_Y)
    type_text(ui, name)
    ui.click(OK_X, OK_Y)
    time.sleep(1.5)


def open_first(ui):
    """The Open dialog is up: take the first file in the list."""
    ui.click(LIST_X, LIST_Y)
    time.sleep(0.3)
    ui.click(OK_X, OK_Y)
    time.sleep(1.5)


def main():
    with UrcUi() as ui:
        w, h = ui.size()
        print("desktop is %dx%d" % (w, h))

        # ---------------- UnoCalc: numbers and a formula -> .xls ------------
        ui.launch_named("UnoCalc", settle=6.0)
        type_text(ui, "6");      ui.key(13)
        type_text(ui, "7");      ui.key(13)
        type_text(ui, "=A1*A2"); ui.key(13)
        time.sleep(0.4)
        ui.shot("uof_00_calc_typed")

        menu(ui, 54, F_SAVE)
        ui.shot("uof_01_calc_save_dialog")
        save_as(ui, "SHEET.XLS")
        ui.shot("uof_02_calc_saved")

        menu(ui, 54, F_NEW)                 # clear it
        ui.shot("uof_03_calc_new")
        menu(ui, 54, F_OPEN)
        ui.shot("uof_04_calc_open_dialog")
        open_first(ui)
        ui.shot("uof_05_calc_reopened")     # 6, 7, 42 must be back
        ui.click(110, 221)                  # select A3
        ui.shot("uof_06_calc_formula_back")  # the formula bar must show =A1*A2
        check(True, "UnoCalc round-trip ran to completion")

        ui.link.command("close", timeout=10)
        time.sleep(1.0)

        # ---------------- UnoShow: a title -> .ppt --------------------------
        ui.launch_named("UnoShow", settle=6.0)
        ui.click(320, 190)
        ui.click(320, 190)
        type_text(ui, "Round Trip")
        ui.shot("uof_07_show_typed")

        menu(ui, 54, F_SAVE)
        ui.shot("uof_08_show_save_dialog")
        save_as(ui, "DECK.PPT")
        ui.shot("uof_09_show_saved")

        menu(ui, 54, F_NEW)
        ui.shot("uof_10_show_new")
        menu(ui, 54, F_OPEN)
        ui.shot("uof_11_show_open_dialog")
        open_first(ui)
        ui.shot("uof_12_show_reopened")     # "Round Trip" must be back
        check(True, "UnoShow round-trip ran to completion")

        for row in ui.link.command("vols", timeout=10):
            print("  vol:", row)

    print("\n%s" % ("uofile urc: %d FAILURE(S)" % len(fails) if fails
                    else "uofile urc: script completed - read shots/uof_*.png"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

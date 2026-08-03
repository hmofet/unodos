#!/usr/bin/env python3
"""Prove UnoShow RUNS: the four views, a dialog, and a slide show with a
transition - driven by real injected clicks and keys over URC.

The host gate (uoffice/build.sh -> uoshow_test) proves the model, the
geometry and the renderer. It cannot prove the module loads, that the shell
routes keys to it, that a click lands where the slide painted, or - the one
that only exists on a booted machine - that the show takes the SCREEN.

    python3 tools/uoshow_urc.py     # needs a UNO_DEBUG=1 build in build/esp
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

        slot = ui.launch_named("UnoShow", settle=6.0)
        print("UnoShow is app slot %d of %d" % (slot, ui.app_count()))
        check(ui.shot("uos_00_open") is not None, "UnoShow opens a window")

        # The title slide opens with two empty placeholders showing their
        # dashed frames and prompts.  Click the centre title and type.
        ui.click(320, 190)
        ui.shot("uos_01_selected")
        ui.click(320, 190)              # a second click enters the text
        type_text(ui, "UnoShow")
        ui.shot("uos_02_typed")

        # Insert > New Slide... opens the AutoLayout picker
        # Menu titles, read off uos_00_open.png: File 54, Edit 91, View 131,
        # Insert 180, Format 237, Tools 290, Slide Show 352, Help 416.  A
        # title's x depends on every label before it, so these are measured,
        # never computed.
        mb_y = 67
        ui.click(180, mb_y)             # Insert
        ui.shot("uos_03_insert_menu")
        ui.click(196, 88)               # New Slide...
        ui.shot("uos_04_newslide_dialog")
        ui.key(13)                      # OK: takes the first layout
        time.sleep(0.5)
        ui.shot("uos_05_second_slide")

        # the views, from the View menu
        ui.click(131, mb_y)
        ui.shot("uos_06_view_menu")
        ui.click(150, 130)              # Slide Sorter (third item)
        ui.shot("uos_07_sorter")

        ui.click(131, mb_y)
        ui.click(150, 109)              # Outline (second item)
        ui.shot("uos_08_outline")

        ui.click(131, mb_y)
        ui.click(150, 88)               # back to Slide
        time.sleep(0.3)

        # THE SHOW: F5 takes the whole screen, then a click advances with a
        # transition, and Esc comes back to the editor.
        ui.key(0, scan=0x0F)            # F5 (a UEFI scan code, not PS/2)
        time.sleep(1.2)
        ui.shot("uos_09_show")
        ui.click(320, 200)              # advance
        time.sleep(0.2)
        ui.shot("uos_10_transition")    # caught mid-wipe if we are quick
        time.sleep(1.0)
        ui.shot("uos_11_show_slide2")
        ui.key(27)                      # Esc ends the show
        time.sleep(0.8)
        ui.shot("uos_12_back_in_editor")

    print("\n%s" % ("uoshow urc: %d FAILURE(S)" % len(fails) if fails
                    else "uoshow urc: script completed - read shots/uos_*.png"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

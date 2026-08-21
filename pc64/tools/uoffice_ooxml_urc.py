#!/usr/bin/env python3
"""Prove the Office suite really opens OOXML files ON THE MACHINE.

unodoc's own gate proves the FORMATS: it reads and writes .xlsx/.docx/.pptx,
round-trips them through its own reader, and hands each one to LibreOffice for
an independent verdict.  It says nothing about whether UnoCalc calls the right
opener - the app-side change is a container sniff and a branch, and a branch
that picks the wrong reader looks exactly like a working one until a file
arrives.  That is the gap this closes.

Each app gets one file pushed onto the RAM disk over URC and opened through
its own File > Open dialog, driven by real injected clicks.  The verdict is
the screenshot: a spreadsheet that opened shows its numbers, and one that did
not shows the "not a workbook this build reads" message box instead - two
pictures nobody can confuse.

    python3 tools/uoffice_ooxml_urc.py     # needs a UNO_DEBUG=1 build

EVERYTHING HERE IS DRIVEN BY THE MOUSE, and that is not a stylistic choice.
uodlg does not move focus on Tab, so a keyboard-driven version of this script
sat on the "Look in" volume combo, sent twelve Downs into it, opened a
different volume's PIC.DOC and reported - correctly, and uselessly - that it
is not a workbook.  Ctrl+O does not reach UnoShow at all.  The menus and the
list respond to clicks in every app, so clicks it is.

The fixtures come from unodoc's corpus (`unodoc/test/corpus`); run
`python3 unodoc/test/run_tests.py corpus` once to generate them.
"""
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from urcui import UrcUi

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.normpath(os.path.join(HERE, "..", "..", "unodoc", "test",
                                       "corpus"))

# Screen geometry, all of it read off a screenshot rather than computed: a
# menu title's x depends on the widths of the labels before it, and the
# dialog is centred on the work area.
MENUBAR_Y = 67          # the uochrome menu band, same in all three apps
FILE_X    = 54          # the "File" title
OPEN_Y    = 109         # second item of the File menu (New, Open..., Save)
ROW0_Y    = 110         # first row of the dialog's file list
ROW_H     = 17
LIST_X    = 250
OK_X, OK_Y = 428, 228   # the dialog's Open button

# (app name, fixture, the name it gets on the RAM disk, what to look for)
CASES = [
    ("UnoCalc", "small.xlsx", "SMALL.XLSX", "cells with numbers and text"),
    ("UnoWord", "small.docx", "SMALL.DOCX", "the document's paragraphs"),
    ("UnoShow", "small.pptx", "SMALL.PPTX", "the deck's first slide"),
]

fails = []


def check(cond, label, detail=""):
    print(("  ok   " if cond else "  FAIL ") + label +
          (("  " + detail) if detail and not cond else ""))
    if not cond:
        fails.append(label)


def open_row(ui, row, tag):
    """File > Open, pick list row `row`, click Open.

    The row is picked rather than typed because that is what an Open dialog is
    FOR - not, as this note used to say, because typing was impossible. It was:
    uofile.c mirrored the selected LIST row into the name field on every sync,
    so anything typed was overwritten before it could be painted. Fixed
    2026-08-21 (it now mirrors only on an actual pick), so the field takes
    keys and a harness may type into it if it wants to.
    """
    ui.click(FILE_X, MENUBAR_Y)
    time.sleep(0.4)
    ui.click(FILE_X + 16, OPEN_Y)        # Open...
    time.sleep(1.2)
    ui.shot(tag + "_dialog")
    ui.click(LIST_X, ROW0_Y + ROW_H * row)
    time.sleep(0.5)
    ui.shot(tag + "_picked")             # the name field shows what was picked
    ui.click(OK_X, OK_Y)
    time.sleep(1.8)
    return ui.shot(tag + "_opened")


def main():
    missing = [f for _, f, _, _ in CASES
               if not os.path.exists(os.path.join(CORPUS, f))]
    if missing:
        print("no fixtures: %s" % ", ".join(missing))
        print("run: python3 unodoc/test/run_tests.py corpus")
        return 2

    with UrcUi() as ui:
        w, h = ui.size()
        print("desktop is %dx%d" % (w, h))

        for n, (app, fixture, onbox, what) in enumerate(CASES):
            print("\n== %s <- %s ==" % (app, onbox))
            src = os.path.join(CORPUS, fixture)
            ok = ui.link.push_file(0, onbox, src)
            check(ok, "%s reached the RAM disk (%d bytes)"
                  % (onbox, os.path.getsize(src)))
            if not ok:
                continue

            # Close the previous app first: launch_named proves the app it
            # opened by looking for its window title, and a window left over
            # from the last case is still on screen to be found.
            ui.link.command("close", timeout=10)
            time.sleep(0.8)
            slot = ui.launch_named(app, settle=6.0, tries=12)
            check(slot is not None, "%s opens a window" % app)

            # The RAM disk starts with README.TXT and each case appends one
            # file, so the file just pushed is row n+1 - uofile.c lists the
            # directory in creation order.
            tag = "ooxml_" + onbox.split(".")[1].lower()
            p = open_row(ui, n + 1, tag)
            check(p is not None, "%s opened %s - shot shows %s"
                  % (app, onbox, what))

    print("\n%s" % ("uoffice ooxml: %d FAILURE(S)" % len(fails) if fails
                    else "uoffice ooxml: script completed - read "
                         "shots/ooxml_*.png"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

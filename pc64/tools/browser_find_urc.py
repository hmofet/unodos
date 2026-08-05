#!/usr/bin/env python3
"""browser_find_urc - drive Ctrl-F in the real UI and screenshot the result.

    cd pc64 && UNO_DEBUG=1 ./build.sh && python3 tools/browser_find_urc.py

Shots: find_01_before (page as loaded), find_02_typed (matches highlighted,
"N of M" in the status band), find_03_next (selection advanced), and
find_04_closed (Esc clears the highlights).
"""
import sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from urcui import UrcUi


def goto(ui, loc, settle=2.5):
    ui.key(ord('l'), ctrl=1)
    ui.key(0, scan=0x06, settle=0.05)             # End
    for _ in range(32):
        ui.key(8, settle=0.04)
    ui.text(loc)
    ui.key(13)
    time.sleep(settle)


def open_browser(ui):
    n = ui.app_count()
    for i in range(n - 1, -1, -1):
        try:
            ui.link.command("launch", i, timeout=15)
        except RuntimeError:
            continue
        time.sleep(3.0)
        if any(t.startswith("Browser") for t in ui.windows()):
            return i
        ui.link.command("close", timeout=10)
        time.sleep(0.6)
    raise SystemExit("no app slot opens the Browser")


def main():
    with UrcUi() as ui:
        open_browser(ui)
        goto(ui, "uno:welcome", settle=3.0)
        ui.shot("find_01_before")

        ui.key(ord('f'), ctrl=1)                  # Ctrl-F opens the find bar
        time.sleep(0.4)
        ui.text("page")                           # a word the welcome page has
        time.sleep(1.2)
        ui.shot("find_02_typed")

        ui.key(13); time.sleep(0.8)               # Enter -> next match
        ui.shot("find_03_next")

        ui.key(0, scan=0x17); time.sleep(0.8)     # Esc (UEFI scan) -> close
        ui.shot("find_04_closed")
    print("done - read shots/find_*.png")


if __name__ == "__main__":
    main()

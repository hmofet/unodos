#!/usr/bin/env python3
"""browser_engine_urc - drive the uno:engine JS-engine switch in the real UI.

Proves what SPECTEST cannot: the uno:engine page renders, its links walk with
the keyboard link-stepper, clicking `quickjs` actually flips the dispatch, and
the JavaScript demo still runs afterwards. Screenshots land in shots/.

    cd pc64 && UNO_DEBUG=1 ./build.sh && python3 tools/browser_engine_urc.py

Key encodings (the URC `key` verb): unicode for characters (13=Enter), UEFI
scan codes for arrows (0x03=Right), ctrl=1 for the Ctrl chord - URC carries
ctrl but not alt.
"""
import sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from urcui import UrcUi


def goto(ui, loc, settle=2.0):
    ui.key(ord('l'), ctrl=1)          # Ctrl-L: focus the address bar
    # the field has no select-all and the caret lands at the END of the
    # current location, so typing would APPEND - clear it first
    ui.key(0, scan=0x06, settle=0.05)             # End (UEFI scan)
    for _ in range(32):
        ui.key(8, settle=0.04)                    # Backspace (unicode 8)
    ui.text(loc)
    ui.key(13)                        # Enter: navigate
    time.sleep(settle)


def open_browser(ui):
    """launch_named() dies on the first refused slot, and the shell REFUSES
    EX_PYAPP/EX_USERAPP - two slots that sit exactly in the from-the-end
    range it searches. So search tolerantly: skip refusals, verify by the
    window title that actually appeared."""
    n = ui.app_count()
    for i in range(n - 1, -1, -1):
        try:
            ui.link.command("launch", i, timeout=15)
        except RuntimeError:
            continue                   # an unlaunchable slot, not a failure
        time.sleep(3.0)
        if any(t.startswith("Browser") for t in ui.windows()):
            return i
        ui.link.command("close", timeout=10)
        time.sleep(0.6)
    raise SystemExit("no app slot opens the Browser")


def main():
    with UrcUi() as ui:
        open_browser(ui)
        goto(ui, "uno:engine")
        ui.shot("engine_01_page_default")          # says "running on unojs"

        ui.key(0, scan=0x03)          # Right: select link 1 (unojs)
        ui.key(0, scan=0x03)          # Right: select link 2 (quickjs)
        ui.key(13)                    # follow -> js_engine_set + rerender
        time.sleep(1.5)
        ui.shot("engine_02_switched")              # says "running on quickjs"

        goto(ui, "uno:script", settle=3.0)         # the <script> demo now
        ui.shot("engine_03_script_on_quickjs")     # runs on quickjs

        goto(ui, "uno:engine", settle=1.5)
        ui.shot("engine_04_still_quickjs")         # the choice persisted
    print("done - read the four shots/engine_*.png")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""logview_urc - open LOGVIEW.UNO and prove it renders the log.

Reads pixels, not the app's own claims: a viewer that opened a window and drew
nothing looks identical over URC to one that worked, which is the failure this
session already hit twice in the browser.

    cd pc64 && UNO_DEBUG=1 ./build.sh
    python3 tools/logview_urc.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from urcui import UrcUi

# The viewer's own colours, from LOGVIEW.PY. Finding them on screen is proof
# the app's draw() ran - nothing else on this desktop paints them.
C_BAD  = (0xE0, 0x38, 0x38)        # emerg..err, as RGB
C_DIM  = (0x90, 0x90, 0x90)


def frame(ui):
    w, h, rgba = ui.link.screen_grab(1, timeout=60)
    return w, h, [(rgba[i], rgba[i+1], rgba[i+2]) for i in range(0, len(rgba), 4)]


def count(px, want, tol=10):
    return sum(1 for p in px
               if abs(p[0]-want[0]) <= tol and abs(p[1]-want[1]) <= tol
               and abs(p[2]-want[2]) <= tol)


def py(ui, s, t=60):
    return ui.link.command("py", s, timeout=t)


def main():
    results = []
    with UrcUi() as ui:
        # Seed the log with something worth looking at, including an ERR so the
        # severity colouring has a line to colour.
        py(ui, "import uno; uno.log_level(7)")
        for i in range(6):
            py(ui, "import uno; uno.log(6,1,'viewer sees this info line %d')" % i)
        py(ui, "import uno; uno.log(3,2,'viewer sees this ERROR line')")

        # A PYAPP is not in the shell's fixed app list - it is hosted in the
        # EX_PYAPP window, which is what Files does when you open a .UNO. Same
        # entry point here, via uno.run_app.
        for v in (1, 2, 0, 3):
            r = py(ui, "import uno; print(uno.run_app(%d,'APPS'+chr(92)+'LOGVIEW.UNO'))" % v)
            if any("0" == l.strip() for l in r):
                break
        time.sleep(3.0)
        wins = ui.windows()
        opened = 0 if any("LOGVIEW" in t.upper() for t in wins) else -1
        print("  windows:", wins)
        results.append(("LOGVIEW opens a window", opened >= 0))

        if opened >= 0:
            time.sleep(2.0)
            ui.shot("logview_01")
            w, h, px = frame(ui)
            bad, dim = count(px, C_BAD), count(px, C_DIM)
            print("  err-red %d px, dim-grey %d px" % (bad, dim))
            results.append(("it draws the ERR line in the severity colour",
                            bad >= 15))
            results.append(("it draws the footer", dim >= 40))

    print()
    n_bad = 0
    for name, ok in results:
        print(("pass " if ok else "FAIL ") + name)
        n_bad += 0 if ok else 1
    print("\n%d pass, %d fail" % (len(results) - n_bad, n_bad))
    return 1 if n_bad else 0


sys.exit(main())

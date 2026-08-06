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
C_BAD  = (200, 40, 40)             # logview.c sev_col(), emerg..err


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

        # It has its own desktop slot now, so it launches like any other app.
        # launch_named dies on the shell's refused slots (EX_PYAPP/EX_USERAPP
        # sit in the from-the-end search range), so search tolerantly.
        n = ui.app_count()
        opened = -1
        for i in range(n - 1, -1, -1):
            try:
                ui.link.command("launch", i, timeout=20)
            except RuntimeError:
                continue
            time.sleep(2.0)
            if any("System Log" in t for t in ui.windows()):
                opened = i
                break
            try:
                ui.link.command("close", timeout=10)
            except RuntimeError:
                pass
            time.sleep(0.4)
        print("  opened from slot", opened, "windows:", ui.windows())
        # The TITLE is the point of the native module: a PYAPP could only be
        # called LOGVIEW.UNO, after its file.
        results.append(("the viewer opens from the desktop, titled \"System Log\"",
                        opened >= 0))

        if opened >= 0:
            time.sleep(2.0)
            ui.shot("logview_01")
            w, h, px = frame(ui)
            bad = count(px, C_BAD)
            # The footer is theme-coloured (text_dim), so count INK in the band
            # rather than a literal - a palette swap must not fail this.
            foot = sum(1 for y in range(h - 120, h - 40)
                       for x in range(60, min(560, w))
                       if px[y * w + x] != (255, 255, 255)
                       and not (px[y*w+x][0] > 240 and px[y*w+x][1] > 240
                                and px[y*w+x][2] > 240))
            print("  err-red %d px, footer ink %d px" % (bad, foot))
            results.append(("it draws the ERR line in the severity colour",
                            bad >= 15))
            results.append(("it draws the footer", foot >= 200))

    print()
    n_bad = 0
    for name, ok in results:
        print(("pass " if ok else "FAIL ") + name)
        n_bad += 0 if ok else 1
    print("\n%d pass, %d fail" % (len(results) - n_bad, n_bad))
    return 1 if n_bad else 0


sys.exit(main())

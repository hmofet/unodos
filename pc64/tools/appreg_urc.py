#!/usr/bin/env python3
"""appreg_urc - prove a .UNO nobody compiled a slot for is a real desktop app.

    UNO_DEBUG=1 ./build.sh && python3 tools/appreg_urc.py

The case is APPS\\VMGR.UNO ("Appliances").  It has no EX_ slot, no icon row, no
name-table entry and no dispatch case anywhere in pc64_uui.c, and before the app
registry it could not be run by ANY means: Files refused it (a unoui-class
module returns a UnoUuiApp, and pc64_shell_run_user only knew PYAPP and classic),
`uno.run_app` does not exist, and URC `launch <n>` indexes shell slots.  If it
opens now, it opened on the strength of the descriptor inside the file.

Driven over URC rather than through QMP because QEMU's usb-tablet delivers no
pointer motion to this guest (see tools/urcui.py), and by NAME rather than by
slot index because the index of a discovered app is by definition not knowable
at test-writing time - which is the same reason every `down`-counting scene in
harness.py drifts the moment an app is added.
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from urcui import UrcUi                                   # noqa: E402

WANT = "Appliances"
fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


with UrcUi() as ui:
    n = ui.app_count()
    print("app slots: %d" % n)
    check(n > 24, "the registry grew past the %d built-in slots" % 24)

    ui.shot("appreg_desktop")

    # search generously: the discovered rows are at the END of the table, but
    # how many there are depends on what is installed, which is the point.
    idx = ui.launch_named(WANT, tries=max(4, n - 24 + 2))
    print("'%s' opened from slot %d" % (WANT, idx))
    check(True, "a discovered module opened a window titled '%s'" % WANT)
    check(idx >= 24, "it is a DISCOVERED row (index %d, past the built-ins)" % idx)

    titles = ui.windows()
    check(any(t.startswith(WANT) for t in titles),
          "its window is titled by the module, not by the file")
    print("windows open: %s" % ", ".join(titles))
    ui.shot("appreg_open")

print("\n%d checks failed" % len(fails))
sys.exit(1 if fails else 0)

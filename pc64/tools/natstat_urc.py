#!/usr/bin/env python3
"""Gate: the System window's STORAGE section shows the detach preflight row.

uno_fat_native_status() (fat.c) answers "would the volume carrying the system
still be there after ExitBootServices, and on which controller" using PCI
config reads only.  The shell puts it under STORAGE, one row below the
"Native FS:" line, because the PRODUCTION build is what an operator runs and
uno_dbg_log compiles to nothing there - the System window is the only surface
production has.

A long row that runs off the edge of the window is the failure this catches:
the desktop is 640x400 under QEMU, narrower than any of the laptops, so if the
line fits here it fits everywhere.  Needs UNO_DEBUG=1 ./build.sh.
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from urcui import UrcUi

with UrcUi() as ui:
    w, h = ui.size()
    n = ui.app_count()
    print("desktop %dx%d, %d app slots" % (w, h, n))
    # Search EVERY slot, not the last few: urcui.launch_named() only looks at
    # the tail because the apps it was written for live there, and System does
    # not - it answered "no-app" from an index past the end rather than saying
    # it had run out of places to look.
    opened = None
    for i in range(n):
        try:
            ui.link.command("launch", i, timeout=15)
        except Exception:
            continue
        import time; time.sleep(1.2)
        titles = ui.windows()
        if any(t.startswith("System") for t in titles):
            opened = i
            break
        try:
            ui.link.command("close", timeout=10)
        except Exception:
            pass
        time.sleep(0.4)
    if opened is None:
        raise SystemExit("FAIL: no app slot opens a window titled System")
    print("System is slot %d; windows: %s" % (opened, ui.windows()))
    ui.shot("sys_storage_top")
    # STORAGE is below the fold: the list shows ten rows and TIMING/INPUT fill
    # them. Focus it and walk down - the point of the shot is the row, not the
    # window, and a screenshot of the part that was always visible proves
    # nothing.
    ui.click(320, 240)                       # inside the list
    for _ in range(14):
        ui.key(0, scan=0x02, settle=0.10)    # UI_KEY_DOWN
    ui.shot("sys_storage_preflight")
    print(">> shots: sys_storage_top.png, sys_storage_preflight.png")

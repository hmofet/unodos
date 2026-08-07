#!/usr/bin/env python3
"""appreg_p5_urc - the last of the registry: APPS.CFG, pinning, custom icons.

    UNO_DEBUG=1 ./build.sh && python3 tools/appreg_p5_urc.py

Three things a person should be able to do to an app they installed WITHOUT
editing anybody's .UNO, and one thing the app itself should be able to do that
the kernel could not have anticipated:

  - rename it            APPS.CFG  name.<id>=
  - pin it to the bar    APPS.CFG  pin.<id>=1
  - hide it              APPS.CFG  hide.<id>=1
  - ship its own icon    `icon: file:NAME.QOI` in its descriptor

The screenshots are the point for the last two: a pinned app that is NOT running
still has a chip, and the Appliances emblem is art decoded from APPS\\VMGR.QOI
rather than anything pc64_icons.c can draw.
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from urcui import UrcUi                                    # noqa: E402

ESP = os.path.join(HERE, "..", "build", "esp")
CFG = os.path.join(ESP, "APPS.CFG")

OVERRIDES = ("name.vmgr=Virtual Machines\r\n"
             "pin.vmgr=1\r\n"
             "hide.uoshow=1\r\n")

fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


existing = open(CFG, "rb").read() if os.path.exists(CFG) else None
open(CFG, "wb").write(OVERRIDES.encode())
print("planted APPS.CFG:\n" +
      "\n".join("       " + l for l in OVERRIDES.splitlines()))

try:
    with UrcUi() as ui:
        apps = dict(ui.apps())
        check(apps.get("vmgr") == "Virtual Machines",
              "name.vmgr renamed the app (%r)" % apps.get("vmgr"))
        check("uoshow" in apps,
              "a hidden app is still REGISTERED (hiding is not uninstalling)")
        # the pinned app is not running, and should still have a taskbar chip
        check(not any(t.startswith("Virtual") for t in ui.windows()),
              "the pinned app is not running")
        ui.shot("appreg_pinned")          # <- a chip with no window behind it
        # ...and it is still launchable under its own id, whatever it is called
        ui.launch_id("vmgr")
        check(any(t.startswith("Virtual Machines") for t in ui.windows()),
              "the renamed app opens under its id and its NEW title")
        check(not any(t.startswith("UnoShow") for t in ui.windows()),
              "the hidden app did not open itself")
        ui.shot("appreg_overrides")
finally:
    if existing is None:
        try:
            os.remove(CFG)
        except OSError:
            pass
    else:
        open(CFG, "wb").write(existing)

print("\n%d checks failed" % len(fails))
sys.exit(1 if fails else 0)

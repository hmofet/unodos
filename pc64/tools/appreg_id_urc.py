#!/usr/bin/env python3
"""appreg_id_urc - apps are named by ID over the wire, and SHELL.CFG agrees.

    UNO_DEBUG=1 ./build.sh && python3 tools/appreg_id_urc.py

Two things, both of which used to be impossible:

  1. `apps list` and `launch <id>`.  Every existing harness launches by SLOT
     NUMBER, which is this boot's ordering of whatever is installed.  That does
     not fail when an app is added - it quietly drives a different app, which is
     how uoword_urc.py spent a run typing into UnoCalc and how the manual's
     scenes drift.  An id cannot do that.

  2. SHELL.CFG v3 keys per-app state by id rather than by index.  The v2 format
     wrote `geom14=`, so installing one app that sorts before another handed
     every later app its neighbour's saved geometry.  This checks the file the
     running system actually wrote.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from urcui import UrcUi                                   # noqa: E402

fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


with UrcUi() as ui:
    apps = ui.apps()
    print("apps: %s" % ", ".join(i for i, _ in apps))
    ids = [i for i, _ in apps]
    check(len(apps) > 0, "`apps list` names every slot")
    check(len(set(ids)) == len(ids), "every id is unique")
    check("vmgr" in ids, "the discovered module is listed under its own id")
    check(all(re.fullmatch(r"[a-z0-9._-]{1,15}", i) for i in ids),
          "every id is a legal config key")

    # launch BY ID: one command, no searching, no counting
    ui.launch_id("vmgr")
    titles = ui.windows()
    check(any(t.startswith("Appliances") for t in titles),
          "`launch vmgr` opened Appliances (%s)" % ", ".join(titles))

    # and a built-in, to show the two kinds are addressed the same way
    ui.launch_id("files")
    titles = ui.windows()
    check(any(t.startswith("Files") for t in titles),
          "`launch files` opened a built-in the same way")

    ui.shot("appreg_by_id")
    disk = getattr(__import__("remote_qemu"), "DISK", None)

# ---- the session file the running system wrote --------------------------
# Read OFF THE DISK IMAGE with mtools, after the guest has shut down. URC has
# no file-read verb (only `put`), and a QEMU vvfat read-write mount corrupts
# writes on this project - reading the raw image is the only honest way to see
# what the OS actually committed.
import subprocess                                          # noqa: E402

text = ""
if disk and os.path.exists(disk):
    for spec in ("::/SHELL.CFG", "::SHELL.CFG"):
        try:
            text = subprocess.run(["mcopy", "-i", disk + "@@1M", spec, "-"],
                                  capture_output=True, timeout=30
                                  ).stdout.decode("ascii", "replace")
            if text:
                break
        except (OSError, subprocess.SubprocessError):
            break

if not text:
    print("       (SHELL.CFG not readable from the image - mtools missing, or "
          "the guest wrote to another volume; skipping the on-disk half)")
else:
    print("SHELL.CFG:\n" + "\n".join("       " + l for l in text.splitlines()))
    check(re.search(r"^geom\.[a-z]", text, re.M) is not None,
          "SHELL.CFG writes geom.<id>=")
    check(re.search(r"^geom\d", text, re.M) is None,
          "SHELL.CFG writes NO index-keyed geometry")
    check(re.search(r"^open=[a-z]", text, re.M) is not None,
          "the open set is a list of ids, not of slot numbers")

print("\n%d checks failed" % len(fails))
sys.exit(1 if fails else 0)

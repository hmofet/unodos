#!/usr/bin/env python3
"""appreg_v2_urc - a v2 SHELL.CFG still restores, and migrates to v3 exactly once.

    UNO_DEBUG=1 ./build.sh && python3 tools/appreg_v2_urc.py

SHELL.CFG v2 keyed every per-app value by SLOT INDEX (`open=0,2`, `geom0=`).
v3 keys them by the app's id, because the app list is discovered at runtime now
and an index means a different app the moment anything is installed. Existing
machines have v2 files, so the reader has to understand both, and the frozen
index->id map (kV2Slots in pc64_uui.c) is the thing that could silently be
wrong - a mistake there does not crash, it restores the WRONG app's geometry.

So: plant a v2 file with distinctive numbers, boot, and check that (a) the apps
it names came back, and (b) the file the system wrote on the way is v3 carrying
those same numbers under the right ids.
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remote_qemu as RQ                                   # noqa: E402
from urcui import UrcUi                                    # noqa: E402

ESP = os.path.join(HERE, "..", "build", "esp")
CFG = os.path.join(ESP, "SHELL.CFG")

# v2, by index: slot 0 = Control Panel, slot 2 = Files (kV2Slots).
V2 = ("restore=1\r\n"
      "open=0,2\r\n"
      "geom0=40,20,300,180\r\n"
      "snap0=0\r\n"
      "geom2=200,60,280,200\r\n"
      "snap2=0\r\n")

fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


existing = open(CFG, "rb").read() if os.path.exists(CFG) else None
open(CFG, "wb").write(V2.encode())
print("planted a v2 SHELL.CFG in build/esp")

try:
    with UrcUi() as ui:
        titles = ui.windows()
        print("restored windows: %s" % ", ".join(titles))
        check(any(t.startswith("Control") for t in titles),
              "v2 slot 0 restored the Control Panel")
        check(any(t.startswith("Files") for t in titles),
              "v2 slot 2 restored Files")
        # any launch triggers session_save, which writes v3
        ui.launch_id("clock")
        ui.shot("appreg_v2_migrated")
finally:
    if existing is None:
        try:
            os.remove(CFG)
        except OSError:
            pass
    else:
        open(CFG, "wb").write(existing)

text = ""
for spec in ("::/SHELL.CFG", "::SHELL.CFG"):
    try:
        text = subprocess.run(["mcopy", "-i", RQ.DISK + "@@1M", spec, "-"],
                              capture_output=True, timeout=30
                              ).stdout.decode("ascii", "replace")
        if text:
            break
    except (OSError, subprocess.SubprocessError):
        break

if not text:
    print("FAIL: could not read SHELL.CFG back off the image")
    sys.exit(1)

print("SHELL.CFG after migration:\n" +
      "\n".join("       " + l for l in text.splitlines()))
check(re.search(r"^geom\d", text, re.M) is None,
      "no index-keyed key survives the rewrite")
check(re.search(r"^geom\.control=40,20,300,180", text, re.M) is not None,
      "v2 geom0 became geom.control with the SAME rect")
check(re.search(r"^geom\.files=200,60,280,200", text, re.M) is not None,
      "v2 geom2 became geom.files with the SAME rect")
check(re.search(r"^open=[a-z]", text, re.M) is not None,
      "the open set is rewritten as ids")

print("\n%d checks failed" % len(fails))
sys.exit(1 if fails else 0)

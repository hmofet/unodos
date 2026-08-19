#!/usr/bin/env python3
"""sync_duum.py - pull the Duum engine from its upstream repository.

Duum is developed at https://github.com/hmofet/duum and vendored here as the
generated single file `pc64/apps/DUUM.PY`.  See `pc64/DUUM-UPSTREAM.md`.

    python pc64/tools/sync_duum.py --from ../duum      # a local checkout
    python pc64/tools/sync_duum.py --ref v0.1.0        # a published tag
    python pc64/tools/sync_duum.py --ref master --dry-run

Standard library only, so it runs wherever the rest of the pc64 tooling does.

After a sync, RUN THE GATES - this is a code drop from another repository:

    python tools/duum_verify.py --wad wads/DOOM1.WAD       # 0 failing views
    python tools/duum_golden.py check --wad wads/DOOM1.WAD # 54/54 identical
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PC64 = os.path.dirname(HERE)
DEST = os.path.join(PC64, "apps", "DUUM.PY")

UPSTREAM = "https://github.com/hmofet/duum"
RAW = "https://raw.githubusercontent.com/hmofet/duum/%s/dist/unodos/DUUM.PY"
VENDORED = os.path.join("dist", "unodos", "DUUM.PY")


def from_local(path):
    src = os.path.join(os.path.abspath(path), VENDORED)
    if not os.path.isfile(src):
        sys.exit("not there: %s\n"
                 "Is %s a checkout of %s?  If it is, run its build first:\n"
                 "    python tools/build.py"
                 % (src, path, UPSTREAM))
    with open(src, encoding="utf-8") as f:
        return f.read(), src


def from_ref(ref):
    import urllib.request
    url = RAW % ref
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            return r.read().decode("utf-8"), url
    except Exception as e:
        sys.exit("could not fetch %s\n  %r" % (url, e))


def describe(text):
    m = re.search(r"^# Duum (\S+) - GENERATED FILE", text, re.M)
    return m.group(1) if m else "unknown"


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--from", dest="src", metavar="PATH",
                   help="a local checkout of the duum repo")
    g.add_argument("--ref", metavar="TAG",
                   help="a tag, branch or commit to fetch from GitHub")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would change and write nothing")
    args = ap.parse_args()

    text, origin = (from_local(args.src) if args.src else from_ref(args.ref))

    # It must be the generated UnoDOS build, not the desktop one and not the
    # package source: the device has no tkinter and no reference rasteriser,
    # and a wrong file here would fail late, on the device.
    if "GENERATED FILE" not in text.split("\n", 1)[0]:
        sys.exit("that is not a generated Duum build (no banner).\n"
                 "Expected upstream's %s" % VENDORED)
    if "import uno" not in text:
        sys.exit("that build does not import `uno`; it is not the UnoDOS one")
    try:
        compile(text, "DUUM.PY", "exec")
    except SyntaxError as e:
        sys.exit("upstream file does not parse: %s" % e)

    old = ""
    if os.path.isfile(DEST):
        with open(DEST, encoding="utf-8") as f:
            old = f.read()

    print("  from     %s" % origin)
    print("  version  %s  (here: %s)" % (describe(text), describe(old) or "-"))
    if old == text:
        print("  result   already identical, nothing to do")
        return 0
    if args.dry_run:
        print("  result   WOULD update %s (%+d lines)"
              % (DEST, text.count("\n") - old.count("\n")))
        return 0

    with open(DEST, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("  result   updated %s (%+d lines)"
          % (DEST, text.count("\n") - old.count("\n")))
    print("\n  Now run the gates - this came from another repository:")
    print("    python tools/duum_verify.py --wad wads/DOOM1.WAD")
    print("    python tools/duum_golden.py check --wad wads/DOOM1.WAD")
    return 0


if __name__ == "__main__":
    sys.exit(main())

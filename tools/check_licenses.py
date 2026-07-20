#!/usr/bin/env python3
"""Third-party notice check - run by pc64/build.sh before it stages the ESP.

THIRD-PARTY.md (repo root) is the manifest of every component that belongs
to another entity. The notice document the OS actually ships is
pc64/docs_esp/LICENSES.MD (System > View licenses). This script fails the
build if any manifest key is missing from the shipped notices, so the two
files cannot drift apart: a component added to the manifest without its
notice - or a notice dropped in an edit - stops the build, the same way the
module-import and no-app-code asserts do.

Exit 0 = every manifest key present; 1 = drift (listed); 2 = cannot parse.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "THIRD-PARTY.md")
NOTICES  = os.path.join(ROOT, "pc64", "docs_esp", "LICENSES.MD")


def manifest_keys():
    keys, in_table = [], False
    for line in open(MANIFEST, encoding="utf-8"):
        if line.startswith("| key |"):
            in_table = True
            continue
        if in_table:
            if not line.startswith("|"):
                break
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if cells and cells[0] and not set(cells[0]) <= {"-", " "}:
                keys.append(cells[0])
    return keys


def main():
    if not os.path.exists(MANIFEST):
        print("check_licenses: missing " + MANIFEST); return 2
    if not os.path.exists(NOTICES):
        print("check_licenses: missing " + NOTICES); return 2
    keys = manifest_keys()
    if not keys:
        print("check_licenses: no manifest rows parsed from THIRD-PARTY.md")
        return 2
    text = open(NOTICES, encoding="utf-8").read().lower()
    missing = [k for k in keys if k.lower() not in text]
    if missing:
        print("FAIL: THIRD-PARTY.md components missing from "
              "pc64/docs_esp/LICENSES.MD:")
        for k in missing:
            print("  - " + k)
        print("Every third-party component must carry its notice in the "
              "shipped LICENSES.MD (see THIRD-PARTY.md, rule 2).")
        return 1
    print("check_licenses: %d third-party notices present" % len(keys))
    return 0


if __name__ == "__main__":
    sys.exit(main())

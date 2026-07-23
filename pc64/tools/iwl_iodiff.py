#!/usr/bin/env python3
"""Align the pc64 iwlwifi I/O trace against a Linux iwlwifi ftrace of a WORKING
load on the same card, and print the divergences.

The AX201 F12 bug (firmware loads, both CPUs run, then park before ALIVE) has
survived many rounds of reading the load path one register at a time and
"verifying" it against recalled Linux source.  This tool replaces that with a
diff: capture what the driver ACTUALLY did on metal, capture what Linux ACTUALLY
did on the same silicon, align the two streams, and let the differences fall out.

Inputs
  ours    the URC session log containing an "iwl iotrace" dump; the lines look
          like   wifi: IOT w32 000024 0804000d x3
  theirs  an ftrace with the iwlwifi_dev_ioread32 / iowrite8 / iowrite32 /
          iowrite64 / ioread_prph32 / iowrite_prph32 events

Both sides are run-length folded (a poll loop becomes one row with xN) so the
alignment is over distinct accesses rather than thousands of identical reads.

  python3 iwl_iodiff.py session.log iwl_from_yoga.txt
  python3 iwl_iodiff.py session.log iwl_from_yoga.txt --kick 2 --before 300
"""
import argparse, re, sys, difflib

# ---- our side -------------------------------------------------------------
OURS = re.compile(r"IOT\s+(r32|w32|w8_|prr|prw)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+x(\d+)")

def parse_ours(path):
    ops, seen = [], False
    for line in open(path, errors="replace"):
        if "IOTRACE begin" in line:
            ops, seen = [], True          # keep only the LAST dump in the log
            continue
        if "IOTRACE end" in line:
            continue
        m = OURS.search(line)
        if m:
            seen = True
            ops.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4))))
    if not seen:
        sys.exit("no IOT lines in %s - run the \"iwl iotrace\" verb on a UNO_DEBUG build" % path)
    return ops

# ---- their side -----------------------------------------------------------
# iwlwifi_dev_iowrite32: [0000:00:14.3] write io[0x24] = 0xc04000d)
# iwlwifi_dev_iowrite64: [0000:00:14.3] write io[64] = 4274511872)   <- DECIMAL
# iwlwifi_dev_iowrite_prph32: [0000:00:14.3] write PRPH[0xa05c44] = 0x1)
THEIRS = re.compile(
    r"iwlwifi_dev_(iowrite8|iowrite32|iowrite64|ioread32|iowrite_prph32|ioread_prph32):"
    r"\s*\[[^\]]*\]\s*(?:write|read)\s+(io|PRPH)\[(0x[0-9a-fA-F]+|\d+)\]\s*=\s*(0x[0-9a-fA-F]+|\d+)")

KIND = {"iowrite8": "w8_", "iowrite32": "w32", "ioread32": "r32",
        "iowrite_prph32": "prw", "ioread_prph32": "prr"}

def num(s):
    return int(s, 16) if s.startswith("0x") else int(s, 10)

def parse_theirs(path):
    ops = []
    for line in open(path, errors="replace"):
        m = THEIRS.search(line)
        if not m:
            continue
        ev, space, off, val = m.group(1), m.group(2), num(m.group(3)), num(m.group(4))
        if ev == "iowrite64":
            # the 64-bit context-info kick; our driver issues it as two 32-bit
            # halves (w64_ -> w32 lo, w32 hi), so split it to match
            ops.append(("w32", off, val & 0xFFFFFFFF, 1))
            ops.append(("w32", off + 4, (val >> 32) & 0xFFFFFFFF, 1))
        else:
            ops.append((KIND[ev], off, val, 1))
    if not ops:
        sys.exit("no iwlwifi_dev_io* events in %s" % path)
    return ops

# ---- shared ---------------------------------------------------------------
def fold(ops):
    out = []
    for k, o, v, n in ops:
        if out and out[-1][0] == k and out[-1][1] == o and out[-1][2] == v:
            out[-1][3] += n
        else:
            out.append([k, o, v, n])
    return [tuple(x) for x in out]

def window(ops, kick_index, before, after):
    """Cut the window around the Nth CSR_CTXT_INFO_BA (io[0x40]) kick."""
    kicks = [i for i, (k, o, v, n) in enumerate(ops) if k == "w32" and o == 0x40]
    if not kicks:
        return ops, None
    if kick_index is None or kick_index > len(kicks):
        kick_index = len(kicks)                      # default: the last load
    ki = kicks[kick_index - 1]
    lo = max(0, ki - before)
    hi = min(len(ops), ki + after)
    return ops[lo:hi], ki - lo

def show(op):
    k, o, v, n = op
    return "%s %06x %08x%s" % (k, o, v, (" x%d" % n) if n > 1 else "")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ours"); ap.add_argument("theirs")
    ap.add_argument("--kick", type=int, default=None,
                    help="which ctxt-info kick in the ftrace to centre on (default: last)")
    ap.add_argument("--before", type=int, default=260)
    ap.add_argument("--after", type=int, default=60)
    ap.add_argument("--values", action="store_true",
                    help="align on value too (default aligns on kind+offset and reports value drift)")
    a = ap.parse_args()

    ours = fold(parse_ours(a.ours))
    theirs, _ = window(fold(parse_theirs(a.theirs)), a.kick, a.before, a.after)

    key = (lambda op: op[:3]) if a.values else (lambda op: op[:2])
    ka, kb = [key(o) for o in ours], [key(o) for o in theirs]
    sm = difflib.SequenceMatcher(None, ka, kb, autojunk=False)

    print("ours=%d folded ops   linux=%d folded ops (window)" % (len(ours), len(theirs)))
    print("  MISSING = Linux did it, we did not.  EXTRA = we did it, Linux did not.")
    print("  VALUE   = same access, different data.\n")
    miss = extra = drift = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            for i, j in zip(range(i1, i2), range(j1, j2)):
                if ours[i][2] != theirs[j][2]:
                    drift += 1
                    print("VALUE   %s   ours=%08x linux=%08x" %
                          (show(theirs[j])[:13], ours[i][2], theirs[j][2]))
            continue
        for j in range(j1, j2):
            miss += 1; print("MISSING %s" % show(theirs[j]))
        for i in range(i1, i2):
            extra += 1; print("EXTRA   %s" % show(ours[i]))
    print("\n%d missing, %d extra, %d value mismatches" % (miss, extra, drift))

if __name__ == "__main__":
    main()

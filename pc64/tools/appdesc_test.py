#!/usr/bin/env python3
# ===========================================================================
# appdesc_test.py - the app-descriptor round trip, on the host.
#
#   python3 tools/appdesc_test.py                 (after a build.sh)
#
# Two halves, and the second is the one that matters:
#
#   1. mkuno's VALIDATOR rejects what it promises to reject.  A malformed
#      descriptor has to be a build failure; if it is not, the failure mode is
#      an app that installs itself wrongly on a machine nobody is watching.
#   2. Every shipped APPS\*.UNO carries a descriptor that the KERNEL's reader
#      would parse the same way - and that the reader can reach with the two
#      reads it is allowed, which means desc_rva has to land inside file_size.
#      This half is a re-implementation of pc64_modload.c's parser, deliberately:
#      two independent readers of one format disagree loudly, one reader agrees
#      with itself no matter what it does.
# ===========================================================================
import glob, os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PC64 = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import mkuno                                     # noqa: E402

HDR_FMT = "<IHHIIIIIIQII"
UNO_MAGIC = 0x314F4E55
DESC_MAGIC = 0x50504155
CATS = ("system", "net", "tools", "media", "games", "other")

fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


# ---- the kernel's parser, re-implemented ---------------------------------
def kernel_read_desc(path):
    """What pc64_modload.c's uno_mod_desc_read would produce, using ONLY the
    two reads it is allowed: 48 bytes at 0, then <= 1024 at 48 + desc_rva."""
    with open(path, "rb") as f:
        hdr = f.read(48)
        if len(hdr) < 48:
            return None
        (magic, abi, flags, entry, mem, fsz, nrel, imp_rva, imp_n,
         base, crc, desc_rva) = struct.unpack(HDR_FMT, hdr)
        if magic != UNO_MAGIC:
            return None
        stem = os.path.basename(path).rsplit(".", 1)[0]
        d = {"id": "".join(c for c in stem.lower()
                           if c.isalnum() or c in "._-")[:15],
             "name": (stem[:1] + stem[1:].lower())[:31],
             "cat": "other", "rank": 100, "flags": [], "tier": flags,
             "desc_rva": desc_rva}
        d["short"] = d["name"]
        if not desc_rva or desc_rva >= fsz:
            return d
        f.seek(48 + desc_rva)
        blk = f.read(1024)
        if len(blk) < 8:
            return d
        dmagic, ver, ln = struct.unpack_from("<IHH", blk, 0)
        if dmagic != DESC_MAGIC or ver != 1 or not 8 < ln <= min(1024, len(blk)):
            return d
        body = blk[8:ln].split(b"\0")[0].decode("ascii", "replace")
        got_short = False
        for line in body.split("\n"):
            if ":" not in line:
                continue
            k, v = line.split(":", 1)
            k, v = k.strip(), v.strip()
            if k in ("id", "name", "icon"):
                d[k] = v
            elif k == "short":
                d["short"] = v
                got_short = True
            elif k == "cat" and v in CATS:
                d["cat"] = v
            elif k == "rank" and v.isdigit():
                d["rank"] = int(v)
            elif k == "flags":
                d["flags"] = [x.strip() for x in v.split(",") if x.strip()]
            elif k == "min" and "x" in v.lower():
                w, h = v.lower().split("x", 1)
                if w.isdigit() and h.isdigit():
                    d["min"] = (int(w), int(h))
        if not got_short:
            d["short"] = d["name"]
        return d


# ---- half 1: the validator rejects what it promises to reject -------------
def synth(body, magic=DESC_MAGIC, ver=1, ln=None, extra=b""):
    """A fake .unodesc section image + the args check_desc takes."""
    b = body.encode() + b"\0"
    ln = len(b) + 8 if ln is None else ln
    blk = struct.pack("<IHH", magic, ver, ln) + b + extra
    return bytearray(blk), 0, len(blk)


def rejects(what, *args, **kw):
    img, va, vsz = synth(*args, **kw)
    try:
        mkuno.check_desc(img, va, vsz, "TEST.UNO")
    except SystemExit:
        check(True, "rejected: " + what)
        return
    check(False, "rejected: " + what)


print("mkuno validator")
img, va, vsz = synth("id: ok\nname: Fine\ncat: tools\n")
try:
    mkuno.check_desc(img, va, vsz, "TEST.UNO")
    check(True, "accepts a well-formed descriptor")
except SystemExit as e:
    check(False, "accepts a well-formed descriptor (%s)" % e)

rejects("bad magic", "id: x\n", magic=0xDEADBEEF)
rejects("wrong version", "id: x\n", ver=2)
rejects("a length past the section", "id: x\n", ln=4096)
rejects("an unknown category", "cat: spaceships\n")
rejects("an unknown flag", "flags: singleton,teleport\n")
rejects("an id with a space", "id: my app\n")
rejects("an id over 15 chars", "id: aaaaaaaaaaaaaaaaaa\n")
rejects("a non-numeric rank", "rank: soon\n")
rejects("a rank over 255", "rank: 900\n")
rejects("a malformed min", "min: wide\n")
rejects("a line that is not key: value", "just some prose\n")
rejects("a repeated key", "id: a\nid: b\n")
rejects("a second UNO_APP_DESC block",
        "id: a\n", extra=struct.pack("<IHH", DESC_MAGIC, 1, 12) + b"id: b\0")

# an unknown KEY must NOT be rejected - that is the extension point
img, va, vsz = synth("id: ok\nfuture: whatever\n")
try:
    mkuno.check_desc(img, va, vsz, "TEST.UNO")
    check(True, "accepts an unknown key (forward compatibility)")
except SystemExit:
    check(False, "accepts an unknown key (forward compatibility)")

# ---- half 2: every shipped module -----------------------------------------
esp = os.path.join(PC64, "build", "esp", "APPS")
mods = sorted(glob.glob(os.path.join(esp, "*.UNO")))
print("\nshipped modules in build/esp/APPS (%d)" % len(mods))
check(bool(mods), "the build produced modules to check")

WANT = {                      # id -> (name, cat) for the ones that must be set
    "vmgr":    ("Appliances", "system"),
    "logview": ("System Log", "system"),
    "photos":  ("Photos",     "media"),
    "studio":  ("Studio",     "tools"),
    "uoword":  ("UnoWord",    "tools"),
    "uocalc":  ("UnoCalc",    "tools"),
    "uoshow":  ("UnoShow",    "tools"),
}
seen = {}
for m in mods:
    d = kernel_read_desc(m)
    base = os.path.basename(m)
    if d is None:
        check(False, "%s parses as a module" % base)
        continue
    if d["desc_rva"]:
        # a descriptor must be READABLE: inside the trimmed file image
        with open(m, "rb") as f:
            fsz = struct.unpack(HDR_FMT, f.read(48))[5]
        check(d["desc_rva"] < fsz,
              "%s descriptor is inside the file image" % base)
        check(d["id"] not in seen,
              "%s id '%s' is unique" % (base, d["id"]))
        seen[d["id"]] = base
        print("       %-14s id=%-9s name=%-12s cat=%-7s rank=%d"
              % (base, d["id"], d["name"], d["cat"], d["rank"]))

for want_id, (name, cat) in WANT.items():
    if want_id + ".uno" not in [s.lower() for s in seen.values()] and \
       want_id not in seen:
        # the module may simply not be built in this configuration
        print("       (%s not built in this configuration)" % want_id)
        continue
    d = kernel_read_desc(os.path.join(esp, seen[want_id]))
    check(d["name"] == name, "%s declares name '%s'" % (want_id, name))
    check(d["cat"] == cat, "%s declares cat '%s'" % (want_id, cat))

print("\n%d checks failed" % len(fails))
sys.exit(1 if fails else 0)

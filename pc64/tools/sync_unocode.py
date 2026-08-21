#!/usr/bin/env python3
"""sync_unocode.py - pull the UnoCode editor from its upstream repository.

UnoCode is developed at https://github.com/hmofet/unocode-desktop, in that
repo's `core/` directory, and is vendored here as `pc64/unocode/`.  See
`pc64/UNOCODE-UPSTREAM.md` for why the direction is that way round and what a
sync has to prove.

    python pc64/tools/sync_unocode.py --from ../unocode-desktop   # a checkout
    python pc64/tools/sync_unocode.py --ref main                  # a branch
    python pc64/tools/sync_unocode.py --ref v1.0.0 --dry-run

Standard library only, so it runs wherever the rest of the pc64 tooling does.

AFTER A SYNC, RUN THE GATE.  This is a code drop from another repository, and
that repository's gate CANNOT SEE A pc64 BREAK: it compiles the editor and its
foundations, never the kernel, so a core change can be green there and fail to
compile here.  That has happened.

    cd pc64 && sh tools/gate.sh                    # QUICK=1 for builds only
    python3 unocode/tools/unocode_urc.py           # the 12-scene QEMU drive
"""

import argparse
import fnmatch
import io
import os
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
PC64 = os.path.dirname(HERE)
DEST = os.path.join(PC64, "unocode")

UPSTREAM = "https://github.com/hmofet/unocode-desktop"
TARBALL = UPSTREAM + "/archive/refs/heads/%s.tar.gz"
TARBALL_TAG = UPSTREAM + "/archive/refs/tags/%s.tar.gz"
SRCDIR = "core"                       # where the core lives in that repo

# What comes across, relative to the upstream `core/` directory.  Anything
# matched here is REPLACED wholesale; anything under DEST that is not matched
# and not in OURS is deleted, so a file deleted upstream does not linger here
# being compiled.
TAKE = [
    "uc_*.c",
    "uc_*.h",
    "unocode.h",
    "UNOCODE.md",
    "tools/test.sh",
    "tools/uc_test.c",
    # Platform implementations of the core's seams.  They live in a
    # SUBDIRECTORY upstream because the desktop build globs `core/uc_*.c`, and
    # a pc64 implementation caught by that glob would be compiled on a host
    # with none of its symbols.  #ifdef cannot separate them: the desktop
    # defines UNO_PC64 too.
    "plat/*.c",
    "ext/*",
    "ext/*/*",
    "ext/*/*/*",
]

# What is OURS, lives under pc64/unocode/, and a sync must never touch.  These
# drive a booted UnoDOS, which is the one thing the upstream repo cannot do.
OURS = [
    "tools/unocode_urc.py",
    "build/*",
    "__pycache__/*",
]

BANNER = [
    "VENDORED FILE - DO NOT EDIT HERE.",
    "",
    "UnoCode is developed at " + UPSTREAM + ", in its core/ directory.",
    "An edit made here is lost at the next sync, and until then it silently",
    "forks the editor away from the tree the desktop builds are cut from.",
    "",
    "Change it there; bring it back with pc64/tools/sync_unocode.py.",
    "See pc64/UNOCODE-UPSTREAM.md.",
]

# How to write the banner into each kind of file.  .JSN is JSONC - the loader
# reads comments and trailing commas - so // is legal in a manifest, a theme
# and a snippet file alike.
COMMENT = {
    ".c":    ("/*", " * ", " */"),
    ".h":    ("/*", " * ", " */"),
    ".js":   (None, "// ", None),
    ".jsn":  (None, "// ", None),
    ".sh":   (None, "# ", None),
    ".py":   (None, "# ", None),
    ".md":   (None, "> ", None),
}


def banner_for(path):
    ext = os.path.splitext(path)[1].lower()
    if ext not in COMMENT:
        return None
    open_, lead, close = COMMENT[ext]
    out = []
    if open_:
        out.append(open_)
    for line in BANNER:
        out.append((lead + line).rstrip())
    if close:
        out.append(close)
    if ext == ".md":
        out.append("")                # a blockquote needs a blank line after
    return "\n".join(out) + "\n"


def matches(rel, patterns):
    rel = rel.replace(os.sep, "/")
    return any(fnmatch.fnmatch(rel, p) for p in patterns)


def to_lf(rel, blob):
    """Every text file crosses as LF, whatever the source checkout holds.

    The upstream repo is developed on Windows as well as Linux, and git hands
    out whatever `core.autocrlf` and its .gitattributes agree on - so a sync
    run there can read CRLF out of a working tree whose COMMITTED bytes are LF.
    Writing that through lands CRLF in this repository, where `tools/test.sh`
    then fails on quill as "required file not found", naming neither the file
    nor the real problem.  Normalising here makes a sync produce the same bytes
    from any checkout on any platform.
    """
    if os.path.splitext(rel)[1].lower() not in COMMENT:
        return blob
    return blob.replace(b"\r\n", b"\n")


def collect(root):
    """Every file under `root` that TAKE selects, as {relpath: bytes}."""
    got = {}
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if not matches(rel, TAKE):
                continue
            with open(full, "rb") as f:
                got[rel] = f.read()
    return got


def from_local(path):
    root = os.path.join(os.path.abspath(path), SRCDIR)
    if not os.path.isdir(root):
        sys.exit("not there: %s\n"
                 "Is %s a checkout of %s?" % (root, path, UPSTREAM))
    got = collect(root)
    stamp = _git_head(path) or "(a working tree, not a commit)"
    return got, "%s @ %s" % (os.path.abspath(path), stamp)


def _git_head(path):
    import subprocess
    try:
        out = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=15)
        if out.returncode:
            return None
        head = out.stdout.strip()
        dirty = subprocess.run(["git", "-C", path, "status", "--porcelain"],
                               capture_output=True, text=True, timeout=15)
        return head + (" (DIRTY)" if dirty.stdout.strip() else "")
    except Exception:
        return None


def from_ref(ref):
    import urllib.error
    import urllib.request
    last = None
    for url in (TARBALL % ref, TARBALL_TAG % ref):
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                blob = r.read()
            break
        except Exception as e:                      # try the tag form next
            last = (url, e)
    else:
        sys.exit("could not fetch %s\n  %r" % (last[0], last[1]))

    got = {}
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as tf:
        for m in tf.getmembers():
            if not m.isfile():
                continue
            parts = m.name.split("/", 2)            # <repo>-<ref>/core/<rel>
            if len(parts) < 3 or parts[1] != SRCDIR:
                continue
            rel = parts[2]
            if not matches(rel, TAKE):
                continue
            got[rel] = tf.extractfile(m).read()
    if not got:
        sys.exit("the tarball for %r carried no %s/ directory - wrong ref?"
                 % (ref, SRCDIR))
    return got, "%s @ %s" % (UPSTREAM, ref)


def existing():
    """Every file currently under pc64/unocode that is not OURS."""
    got = set()
    for dirpath, _dirs, files in os.walk(DEST):
        for name in files:
            rel = os.path.relpath(os.path.join(dirpath, name),
                                  DEST).replace(os.sep, "/")
            if not matches(rel, OURS):
                got.add(rel)
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--from", dest="path", metavar="DIR",
                     help="a local checkout of " + UPSTREAM)
    src.add_argument("--ref", metavar="REF",
                     help="a branch or tag to fetch from GitHub")
    ap.add_argument("--dry-run", action="store_true",
                    help="say what would change and write nothing")
    args = ap.parse_args()

    got, where = from_local(args.path) if args.path else from_ref(args.ref)

    # A file that already carries the banner came FROM here, which means the
    # source is a vendored copy and syncing it would be a no-op that looks
    # like a sync.  Say so rather than shuffling bytes.
    for rel, blob in got.items():
        if b"VENDORED FILE - DO NOT EDIT HERE" in blob[:1024]:
            sys.exit("%s in the source already carries the vendored banner.\n"
                     "That is a copy of THIS tree, not the upstream one." % rel)

    # The provenance stamp.  Without it, "which upstream commit is this copy?"
    # is unanswerable from inside this repository, which is exactly the question
    # somebody asks while debugging a sync.
    STAMP = "VENDORED.txt"

    have = existing()
    have.discard(STAMP)
    added, changed, gone = [], [], sorted(have - set(got))

    for rel in sorted(got):
        text = to_lf(rel, got[rel])
        b = banner_for(rel)
        if b:
            text = b.encode("utf-8") + text
        dst = os.path.join(DEST, rel.replace("/", os.sep))
        old = None
        if os.path.isfile(dst):
            with open(dst, "rb") as f:
                old = to_lf(rel, f.read())   # compare like for like: a CRLF
                                             # checkout is not a CHANGE
        if old is None:
            added.append(rel)
        elif old != text:
            changed.append(rel)
        else:
            continue
        if args.dry_run:
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(text)
        if rel.endswith(".sh"):
            os.chmod(dst, 0o755)

    for rel in gone:
        if args.dry_run:
            continue
        os.remove(os.path.join(DEST, rel.replace("/", os.sep)))

    if not args.dry_run:
        with open(os.path.join(DEST, STAMP), "w", encoding="utf-8",
                  newline="\n") as f:
            f.write("UnoCode, vendored from %s\n\n"
                    "source: %s\n"
                    "files:  %d\n\n"
                    "Written by pc64/tools/sync_unocode.py.  Do not edit this\n"
                    "directory here - see pc64/UNOCODE-UPSTREAM.md.\n"
                    % (UPSTREAM, where, len(got)))

    if not args.dry_run:
        for dirpath, dirs, files in os.walk(DEST, topdown=False):
            if not dirs and not files:
                os.rmdir(dirpath)

    print("from: %s" % where)
    print("into: %s" % DEST)
    for label, group in (("added", added), ("updated", changed),
                         ("deleted", gone)):
        for rel in group:
            print("  %-8s %s" % (label, rel))
    if not (added or changed or gone):
        print("  (already identical)")
    print("%d file(s) vendored%s."
          % (len(got), " - DRY RUN, nothing written" if args.dry_run else ""))
    if not args.dry_run and (added or changed or gone):
        print("\nNow prove it - the upstream gate cannot see a pc64 break:")
        print("    cd pc64 && sh tools/gate.sh")
        print("    python3 unocode/tools/unocode_urc.py")


if __name__ == "__main__":
    main()
